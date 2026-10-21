"""Real Textual Pilot coverage of setup presentation and engine handoff.

These tests do not connect to a device, install files or start SteamVR. They
prove UI behavior, not headset rendering or successful device installation.
"""
from __future__ import annotations

import asyncio
import html
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import threading
import time
import unittest
from unittest import mock

from textual.drivers.headless_driver import HeadlessDriver
from textual.widgets import Button, Collapsible, Input, OptionList, Static, TextArea

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("_sfnp_setup_textual", ROOT / "setup/steam_link_setup_textual.py")
UI = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = UI
SPEC.loader.exec_module(UI)
FLOW_SPEC = importlib.util.spec_from_file_location("_sfnp_textual_workflow", ROOT / "setup/steam_link_setup_workflow.py")
FLOW = importlib.util.module_from_spec(FLOW_SPEC)
FLOW_SPEC.loader.exec_module(FLOW)


async def until(pilot, predicate):
    for _ in range(150):
        if predicate():
            return
        await pilot.pause(.01)
    raise AssertionError("the expected UI/worker state was not reached")


class PicturePorts:
    """No device I/O: a real workflow with observable transaction boundaries."""
    error = ValueError

    def __init__(self, *, finish_error=None):
        self.events = []
        self.finish_error = finish_error
        self.stale_health = False

    def begin(self): self.events.append("begin")
    def stop_pc(self): self.events.append("stop_pc")
    def prepare(self, action): self.events.append("prepare")
    def restart_prepared(self): self.events.append("restart_prepared")
    def prepared_health(self):
        self.events.append("prepared_health")
        return {"software_ready": not self.stale_health}
    def restore_files(self): self.events.append("restore_files")
    def restart_original(self): self.events.append("restart_original")
    def original_health(self): self.events.append("original_health"); return {"software_ready": True}
    def finish(self, *, restored):
        self.events.append("finish_original" if restored else "finish_prepared")
        if self.finish_error is not None:
            raise self.finish_error
    def failure(self, error, *, recovery): self.events.append("failure")


class TextualSetupTests(unittest.IsolatedAsyncioTestCase):
    async def test_pc_only_apply_finishes_without_frame_restart_or_picture_prompt(self):
        for lang in ('en','de'):
            ports=PicturePorts();ports.frame_required=False
            def backend():
                p=UI.active_presenter();p.say('bridge_pc_only_plan')
                if not p.confirm('bridge_confirm'):return 130
                return FLOW.Workflow(ports,p).run('update')
            app=UI.SetupApp(backend,lang)
            async with app.run_test(size=(80,24)) as pilot:
                await until(pilot,lambda:isinstance(app.screen,UI.ConfirmScreen))
                self.assertIn(app.texts['bridge_pc_only_plan'],app.screen.plan)
                await until(pilot,lambda:app.focused is not None)
                await pilot.pause()
                self.assertTrue(await pilot.click('#accept'))
                await until(pilot,lambda:app.backend_finished)
                self.assertEqual(app.backend_result,0)
                self.assertEqual(app.outcome,'bridge_pc_only_installed')
                self.assertIn(app.words['connection_installed'],str(app.query_one('#complete-title',Static).render()))
                self.assertEqual(ports.events,['begin','stop_pc','prepare','prepared_health','finish_prepared'])
            app.backend.join()

    async def test_copy_controls_are_compact_secondary_utilities_in_main_view_and_modal(self):
        for lang in ("en", "de"):
            def backend():
                p = UI.active_presenter()
                p.path("C:/Setup/logs/setup-737.jsonl", "setup_log")
                p.choose_action(["exit"])
                return 0
            app = UI.SetupApp(backend, lang)
            async with app.run_test(size=(80, 24)) as pilot:
                await until(pilot, lambda: app.prompt is not None)
                for width, height in ((80, 24), (42, 16)):
                    await pilot.resize_terminal(width, height)
                    # Scroll only after the resized viewport has been laid out.
                    await pilot.pause()
                    app.query_one("#copy-log", Button).focus(scroll_visible=False)
                    app.query_one("#file-buttons").scroll_visible(animate=False)
                    await pilot.pause()
                    row = app.query_one("#file-buttons")
                    self.assertEqual(row.region.height, 1)
                    for button in row.query(Button):
                        self.assertEqual(button.variant, "default")
                        self.assertTrue(button.compact)
                        self.assertEqual(button.region.height, 1)
                        self.assertLess(button.region.width, width // 2)
                        self.assertLessEqual(button.region.width, len(button.label.plain) + 4)
                        self.assertTrue(row.region.contains_region(button.region), (lang, width, button.id))
                        self.assertTrue(app.query_one("#body").content_region.contains_region(button.region))
                        self.assertEqual(button.styles.background.a, 0)
                await pilot.press("f3")
                await until(pilot, lambda: isinstance(app.screen, UI.PathsScreen))
                for width, height in ((80, 24), (42, 16)):
                    await pilot.resize_terminal(width, height)
                    await pilot.pause()
                    row = app.screen.query_one("#paths-buttons")
                    self.assertEqual(row.region.height, 1)
                    for button in row.query(Button):
                        self.assertEqual(button.variant, "default")
                        self.assertTrue(button.compact)
                        self.assertEqual(button.region.height, 1)
                        self.assertLessEqual(button.region.width, len(button.label.plain) + 4)
                        self.assertTrue(row.region.contains_region(button.region), (lang, width, button.id))
                        self.assertEqual(button.styles.background.a, 0)
                await pilot.press("escape", "ctrl+c")
                await until(pilot, lambda: app.backend_finished)
            app.backend.join()

    async def test_file_paths_offer_native_copy_and_readonly_fallback_at_small_sizes(self):
        log = "C:/Users/Player/AppData/Local/Steam Link Native Passthrough/logs/setup-736.jsonl"
        recovery = "C:/Users/Player/AppData/Local/Steam Link Native Passthrough/recovery/checkpoint.json"
        for lang in ("en", "de"):
            def backend():
                p = UI.active_presenter()
                p.path(log, "setup_log")
                p.path(recovery, "recovery_pc")
                p.choose_action(["exit"])
                return 0
            app = UI.SetupApp(backend, lang)
            with mock.patch.object(UI, "_copy_public_paths") as copy:
                async with app.run_test(size=(80, 24)) as pilot:
                    await until(pilot, lambda: app.prompt is not None)
                    await pilot.press("f3")
                    await until(pilot, lambda: isinstance(app.screen, UI.PathsScreen))
                    for width, height in ((80, 24), (42, 16), (80, 24)):
                        await pilot.resize_terminal(width, height)
                        await pilot.pause()
                        dialog = app.screen.query_one("#paths-dialog").region
                        for button in app.screen.query(Button):
                            self.assertTrue(dialog.contains_region(button.region), (lang, width, button.id, dialog, button.region))
                        area = app.screen.query_one("#selected-path", TextArea)
                        self.assertTrue(dialog.contains_region(area.region))
                        self.assertTrue(area.read_only)
                        self.assertEqual(area.text, log)
                    await pilot.press("down")
                    await pilot.pause()
                    self.assertEqual(area.text, recovery)
                    area.focus()
                    await pilot.press("x", "backspace")
                    self.assertEqual(area.text, recovery)
                    await pilot.click("#copy-path")
                    await until(pilot, lambda: copy.call_count == 1 and not app.copy_pending)
                    self.assertEqual(copy.call_args.args, (recovery,))
                    self.assertEqual(str(app.screen.query_one("#copy-status", Static).render()), app.words["copied_path"])
                    await pilot.click("#copy-paths")
                    await until(pilot, lambda: copy.call_count == 2 and not app.copy_pending)
                    all_paths = copy.call_args.args[0]
                    self.assertIn(log, all_paths)
                    self.assertIn(recovery, all_paths)
                    self.assertIn(app.texts["recovery_pc"].strip(), all_paths)
                    await pilot.press("escape", "enter")
                    await until(pilot, lambda: app.backend_finished)
            app.backend.join()

    async def test_failed_clipboard_shows_selectable_path_without_claiming_success(self):
        log = "C:/Setup with spaces/logs/setup-736.jsonl"
        def backend():
            p = UI.active_presenter()
            p.path(log, "setup_log")
            p.choose_action(["exit"])
            return 0
        for lang in ("en", "de"):
            app = UI.SetupApp(backend, lang)
            with mock.patch.object(UI, "_copy_public_paths", side_effect=UI.ClipboardUnavailable("fixture failure")) as copy:
                async with app.run_test(size=(42, 16)) as pilot:
                    await until(pilot, lambda: app.prompt is not None)
                    await pilot.press("f4")
                    await until(pilot, lambda: isinstance(app.screen, UI.PathsScreen) and not app.copy_pending)
                    self.assertEqual(copy.call_args.args, (log,))
                    status = str(app.screen.query_one("#copy-status", Static).render())
                    self.assertEqual(status, app.words["copy_failed"])
                    self.assertNotIn(app.words["copied_path"], status)
                    area = app.screen.query_one("#selected-path", TextArea)
                    self.assertEqual(area.text, log)
                    self.assertTrue(area.read_only)
                    self.assertIs(app.focused, area)
                    for button in app.screen.query(Button):
                        self.assertTrue(app.screen.query_one("#paths-dialog").region.contains_region(button.region))
                    visible = " ".join(html.unescape(re.sub(r"<[^>]+>", " ", app.export_screenshot())).split())
                    self.assertIn("Copy command." if lang == "en" else "Terminal kopieren.", visible)
                    await pilot.press("escape", "enter")
                    await until(pilot, lambda: app.backend_finished)
            app.backend.join()

    async def test_copy_shortcuts_never_read_password_and_ctrl_c_still_aborts(self):
        log = "C:/Setup/logs/setup-736.jsonl"
        observed = []
        def backend():
            p = UI.active_presenter()
            p.path(log, "setup_log")
            observed.append(p.ssh_prompt("Password:", secret=True))
            return 0
        app = UI.SetupApp(backend, "en")
        with mock.patch.object(UI, "_copy_public_paths") as copy, mock.patch.object(app, "copy_to_clipboard") as osc52:
            async with app.run_test(size=(80, 24)) as pilot:
                await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
                field = app.screen.query_one(Input)
                field.value = "fixture-password-must-never-be-copied"
                field.action_select_all()
                await pilot.press("ctrl+shift+c")
                await until(pilot, lambda: copy.call_count == 1 and not app.copy_pending)
                self.assertEqual(copy.call_args.args, (log,))
                await pilot.press("f3")
                self.assertIsInstance(app.screen, UI.SSHInputScreen)
                self.assertEqual(field.value, "fixture-password-must-never-be-copied")
                await pilot.press("ctrl+c")
                await until(pilot, lambda: app.backend_finished)
                self.assertEqual(observed, [None])
                self.assertTrue(app.stop_requested)
                self.assertEqual(app.backend_result, 130)
                self.assertEqual(copy.call_count, 1)
                osc52.assert_not_called()
                self.assertNotIn("fixture-password", "\n".join(app.detail_history))
        app.backend.join()

    async def test_copy_log_without_file_is_explicit_and_does_not_copy_other_ui(self):
        def backend():
            UI.active_presenter().choose_action(["exit"])
            return 0
        app = UI.SetupApp(backend, "en")
        with mock.patch.object(UI, "_copy_public_paths") as copy, mock.patch.object(app, "notify") as notify:
            async with app.run_test(size=(80, 24)) as pilot:
                await until(pilot, lambda: app.prompt is not None)
                await pilot.press("f4")
                copy.assert_not_called()
                self.assertEqual(notify.call_args.args, (app.words["copy_missing"],))
                await pilot.press("f3")
                await until(pilot, lambda: isinstance(app.screen, UI.PathsScreen))
                self.assertTrue(app.screen.query_one("#copy-path", Button).disabled)
                self.assertTrue(app.screen.query_one("#copy-paths", Button).disabled)
                await pilot.press("escape", "enter")
                await until(pilot, lambda: app.backend_finished)
        app.backend.join()

    def test_clipboard_windows_uses_unicode_native_api_and_transfers_memory_only_on_success(self):
        import ctypes
        from types import SimpleNamespace
        value = "C:/Spieler/Änne/Setup/日誌.jsonl"
        for accepted in (True, False):
            buffer = ctypes.create_string_buffer(1024)
            user = SimpleNamespace(**{name: mock.Mock(return_value=result) for name, result in (
                ("GetForegroundWindow", 42), ("OpenClipboard", 1), ("EmptyClipboard", 1),
                ("SetClipboardData", 73 if accepted else 0), ("CloseClipboard", 1))})
            kernel = SimpleNamespace(**{name: mock.Mock(return_value=result) for name, result in (
                ("GlobalAlloc", 73), ("GlobalLock", ctypes.addressof(buffer)), ("GlobalUnlock", 1),
                ("GlobalFree", 0), ("GetConsoleWindow", 41))})
            with mock.patch.object(ctypes, "WinDLL", create=True, side_effect=lambda name, **kw: user if name == "user32" else kernel), \
                    mock.patch.object(UI.sys, "platform", "win32"):
                if accepted:
                    UI._copy_public_paths(value)
                else:
                    with self.assertRaises(UI.ClipboardUnavailable):
                        UI._copy_public_paths(value)
            payload = value.encode("utf-16-le") + b"\0\0"
            self.assertEqual(buffer.raw[:len(payload)], payload)
            user.OpenClipboard.assert_called_once_with(41)
            user.SetClipboardData.assert_called_once_with(13, 73)
            user.CloseClipboard.assert_called_once()
            self.assertEqual(kernel.GlobalFree.call_count, int(not accepted))

    def test_clipboard_linux_uses_local_tool_with_utf8_stdin_without_shell_or_osc52(self):
        value = "/home/Änne/Setup $(not-a-command)/setup-736.jsonl"
        for tool, environment in (("wl-copy", {"WAYLAND_DISPLAY": "wayland-1"}), ("xclip", {"DISPLAY": ":1"})):
            with mock.patch.object(UI.sys, "platform", "linux"), mock.patch.dict(os.environ, environment, clear=True), \
                    mock.patch.object(UI.shutil, "which", side_effect=lambda name: "/usr/bin/" + name if name == tool else None), \
                    mock.patch.object(UI.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
                UI._copy_public_paths(value)
            self.assertEqual(run.call_args.args[0][0], "/usr/bin/" + tool)
            self.assertEqual(run.call_args.kwargs["input"], value.encode("utf-8"))
            self.assertNotIn("shell", run.call_args.kwargs)
            self.assertEqual(run.call_args.kwargs["timeout"], 3)
            self.assertEqual(run.call_args.kwargs["stderr"], subprocess.DEVNULL)

    def test_clipboard_failure_and_invalid_values_do_not_claim_terminal_acceptance(self):
        with mock.patch.object(UI.sys, "platform", "linux"), mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(UI.shutil, "which") as which, mock.patch.object(UI.subprocess, "run") as run:
            for value in ("", "x\0y", "x" * 136193, "a valid path"):
                with self.assertRaises(UI.ClipboardUnavailable):
                    UI._copy_public_paths(value)
            which.assert_not_called()
            run.assert_not_called()

    def test_clipboard_tool_failure_timeout_and_missing_command_use_manual_fallback(self):
        with mock.patch.object(UI.sys, "platform", "linux"), \
                mock.patch.dict(os.environ, {"WAYLAND_DISPLAY": "wayland-1", "DISPLAY": ":1"}, clear=True), \
                mock.patch.object(UI.shutil, "which", side_effect=lambda name: "/usr/bin/" + name), \
                mock.patch.object(UI.subprocess, "run", side_effect=[
                    subprocess.TimeoutExpired("wl-copy", 3), mock.Mock(returncode=1), FileNotFoundError()]) as run:
            with self.assertRaises(UI.ClipboardUnavailable):
                UI._copy_public_paths("/home/player/setup-736.jsonl")
            self.assertEqual(run.call_count, 3)

    async def test_ctrl_c_from_file_paths_cancels_setup_without_copying_or_undo(self):
        selected = []
        def backend():
            selected.append(UI.active_presenter().choose_action(["exit"]))
            return 0
        app = UI.SetupApp(backend, "en")
        with mock.patch.object(UI, "_copy_public_paths") as copy:
            async with app.run_test(size=(42, 16)) as pilot:
                await until(pilot, lambda: app.prompt is not None)
                await pilot.press("f3", "ctrl+c")
                await until(pilot, lambda: app.backend_finished)
                self.assertFalse(isinstance(app.screen, UI.PathsScreen))
                self.assertEqual(app.backend_result, 130)
                self.assertEqual(selected, [None])
                copy.assert_not_called()
        app.backend.join()

    async def test_detected_runtime_versions_and_patch_actions_remain_visible_at_80x24(self):
        for lang in ('en','de'):
            runtime={'steamvr':{'version':'2.18.2','build':'1791331621'},'steam_link':{'version':None,'build':None}}
            def backend():
                p=UI.active_presenter()
                p.inventory(pc={'compatible':False,'installed':True,'installed_version':'0.2.0','target_version':'0.2.1','runtime_versions':runtime},
                            frame={'runtime_compatible':True,'installed':True,'installed_version':'0.2.0','target_version':'0.2.1','runtime_versions':runtime,'host':'192.168.0.25'})
                p.choose_action(['update','uninstall','retry','check','exit'],recommended='update')
                return 0
            app=UI.SetupApp(backend,lang)
            async with app.run_test(size=(80,24)) as pilot:
                await until(pilot,lambda:app.prompt is not None);await pilot.pause()
                body=app.query_one('#body')
                for key in ('pc','frame'):
                    text=str(app.query_one('#'+key+'-status',Static).render())
                    self.assertIn('SteamVR: 2.18.2 (1791331621)',text)
                    self.assertIn('Steam Link: '+app.texts['runtime_bundled'],text)
                    self.assertIn('0.2.0',text);self.assertIn('0.2.1',text)
                    self.assertTrue(body.content_region.contains_region(app.query_one('#'+key+'-status').region), (key, body.content_region, app.query_one('#'+key+'-status').region, body.scroll_y))
                self.assertIn(app.words['attention'],str(app.query_one('#pc-status',Static).render()))
                self.assertGreater(app.query_one(OptionList).region.height,0)
                self.assertTrue(body.content_region.contains_region(app.query_one(OptionList).region))
                for width,height in ((42,16),(100,36),(80,24)):
                    await pilot.resize_terminal(width,height);await pilot.pause()
                    self.assertLessEqual(app.query_one('#frame-card').region.right,width)
                    self.assertIn('SteamVR: 2.18.2',str(app.query_one('#frame-status',Static).render()))
                await pilot.press('end','enter');await until(pilot,lambda:app.backend_finished)
            app.backend.join()

    async def test_component_version_cards_remain_in_compact_viewport_while_actions_scroll(self):
        for lang in ("en","de"):
            def backend():
                p=UI.active_presenter()
                p.inventory(pc={"compatible":True,"installed":True,"installed_version":"0.1.9","target_version":"0.2.0"},
                            frame={"runtime_compatible":True,"installed":True,"installed_version":None,"target_version":"0.2.0","host":"192.168.0.25"})
                p.choose_action(["update","uninstall","retry","check","exit"],recommended="update")
                return 0
            app=UI.SetupApp(backend,lang)
            async with app.run_test(size=(80,24)) as pilot:
                await until(pilot,lambda:app.prompt is not None)
                await pilot.pause()
                body=app.query_one("#body")
                for key in ("pc","frame"):
                    self.assertTrue(body.content_region.contains_region(app.query_one("#"+key+"-card").region),key)
                    self.assertTrue(body.content_region.contains_region(app.query_one("#"+key+"-status").region),key)
                self.assertEqual(body.scroll_y,0)
                await pilot.press("end");await pilot.pause()
                self.assertGreater(app.query_one(OptionList).scroll_y,0)
                self.assertEqual(body.scroll_y,0)
                for key in ("pc","frame"):
                    self.assertTrue(body.content_region.contains_region(app.query_one("#"+key+"-status").region),key)
                app.show_notice("A useful warning remains visible",state="warning")
                await pilot.pause()
                self.assertTrue(app.query_one("#notice").display)
                await pilot.press("enter")
                await until(pilot,lambda:app.backend_finished)
            app.backend.join()

    async def test_untested_warning_is_modal_explicit_and_shows_real_versions(self):
        for lang in ('en','de'):
            for accept in (False,True):
                observed=[]
                pc={'steamvr':{'version':'2.19.1','build':'1800000001'},'steam_link':{}}
                frame={'steamvr':{'version':'2.19.2','build':'1800000002'},'steam_link':{}}
                def backend():
                    p=UI.active_presenter()
                    p.inventory(pc={'compatible':False,'installed':True,'activation_mode':'inactive','runtime_versions':pc},
                                frame={'runtime_compatible':False,'installed':True,'activation_mode':'inactive','runtime_versions':frame})
                    p.untested_runtime(pc=pc,frame=frame)
                    observed.append(p.confirm('bridge_untested_confirm'))
                    if observed[-1]:p.say('bridge_untested_installed')
                    return 0
                app=UI.SetupApp(backend,lang)
                async with app.run_test(size=(80,24)) as pilot:
                    await until(pilot,lambda:isinstance(app.screen,UI.ConfirmScreen))
                    await until(pilot,lambda:app.focused is not None)
                    await pilot.pause()
                    self.assertEqual(app.focused.id,'cancel')
                    self.assertIn('2.19.1 (1800000001)',app.screen.plan)
                    self.assertIn('2.19.2 (1800000002)',app.screen.plan)
                    self.assertIn('Steam Link: '+app.texts['runtime_bundled'],app.screen.plan)
                    self.assertNotIn('Steam Link: '+app.texts['runtime_unknown'],app.screen.plan)
                    self.assertEqual(str(app.screen.query_one('#accept',Button).label),app.texts['bridge_untested_accept'])
                    underlay=app.screen_stack[0]
                    self.assertIn(app.texts['bridge_untested_inactive'],str(underlay.query_one('#frame-status',Static).render()))
                    dialog=app.screen.query_one('#confirm-dialog').region
                    for button in app.screen.query(Button):
                        self.assertTrue(dialog.contains_region(button.region),button.id)
                    if accept:await pilot.click('#accept')
                    else:await pilot.press('enter')
                    await until(pilot,lambda:app.backend_finished)
                    self.assertEqual(observed,[accept])
                    if accept:
                        self.assertEqual(app.outcome,'bridge_untested_installed')
                        self.assertIn(app.words['installed_inactive'],str(app.query_one('#complete-title',Static).render()))
                app.backend.join()

    async def test_component_versions_are_independent_in_status_and_apply_plan(self):
        for lang in ("en","de"):
            observed=[]
            pc={"compatible":True,"installed":True,"installed_version":"0.1.9","target_version":"0.2.0"}
            frame={"runtime_compatible":True,"installed":True,"installed_version":None,"target_version":"0.2.1"}
            def backend():
                p=UI.active_presenter()
                p.inventory(pc=pc,frame=frame,report={"installer_version":"0.2.12"})
                p.say("bridge_install_plan")
                p.version_plan(pc={**pc,"installed_version":"0.0.9"},frame=frame)
                p.version_plan(pc=pc,frame=frame)
                observed.append(p.confirm("bridge_confirm"))
                return 0
            app=UI.SetupApp(backend,lang)
            async with app.run_test(size=(100,36)) as pilot:
                await until(pilot,lambda:isinstance(app.screen,UI.ConfirmScreen))
                underlying=app.screen_stack[0]
                pc_status=str(underlying.query_one("#pc-status",Static).render())
                frame_status=str(underlying.query_one("#frame-status",Static).render())
                self.assertIn("Installer: 0.2.12",str(underlying.query_one("#brand",Static).render()))
                self.assertNotIn("0.2.12",pc_status);self.assertNotIn("0.2.12",frame_status)
                self.assertIn("0.1.9",pc_status);self.assertIn("0.2.0",pc_status)
                self.assertNotIn("0.1.9",frame_status);self.assertIn("0.2.1",frame_status)
                self.assertIn(app.texts["version_unknown"],frame_status)
                self.assertIn("0.1.9 → 0.2.0",app.screen.plan)
                self.assertNotIn("0.0.9",app.screen.plan)
                self.assertEqual(app.screen.plan.count(app.texts["version_plan"]),1)
                self.assertIn(app.texts["version_unknown"]+" → 0.2.1",app.screen.plan)
                self.assertIn(app.texts["version_pc"],app.screen.plan)
                self.assertIn(app.texts["version_frame"],app.screen.plan)
                await pilot.resize_terminal(80,24);await pilot.pause()
                dialog=app.screen.query_one("#confirm-dialog").region
                for button in app.screen.query(Button):
                    self.assertTrue(dialog.contains_region(button.region),button.id)
                visible=" ".join(html.unescape(re.sub(r"<[^>]+>"," ",app.export_screenshot())).split())
                self.assertIn("0.1.9 → 0.2.0",visible)
                self.assertIn(app.texts["version_unknown"]+" → 0.2.1",visible)
                await pilot.press("escape")
                await until(pilot,lambda:app.backend_finished)
                self.assertEqual(observed,[False])
            app.backend.join()

    async def test_component_version_status_distinguishes_not_checked_missing_and_unversioned(self):
        ready=threading.Event();release=threading.Event()
        def backend():
            p=UI.active_presenter()
            p.inventory(pc={"installed":False,"installed_version":None,"target_version":"0.2.0"},
                        frame={"installed":None,"installed_version":None,"target_version":"0.2.0"})
            ready.set();release.wait();return 0
        app=UI.SetupApp(backend,"en")
        try:
            async with app.run_test(size=(80,24)) as pilot:
                await until(pilot,ready.is_set)
                self.assertIn("Installed patch: Not installed",str(app.query_one("#pc-status",Static).render()))
                self.assertIn("Installed patch: Not checked",str(app.query_one("#frame-status",Static).render()))
                self.assertNotIn("Not installed",str(app.query_one("#frame-status",Static).render()))
                app.update_inventory(frame={"installed":True,"installed_version":None,"target_version":"0.2.0"})
                self.assertIn("Installed patch: Version unknown",str(app.query_one("#frame-status",Static).render()))
                for width,height in ((42,16),(100,36)):
                    await pilot.resize_terminal(width,height);await pilot.pause()
                    self.assertLessEqual(app.query_one("#frame-card").region.right,width)
                    self.assertIn("Available patch: 0.2.0",str(app.query_one("#frame-status",Static).render()))
                release.set();await until(pilot,lambda:app.backend_finished)
        finally:release.set();app.backend.join()

    async def test_picture_popup_retains_setup_background_and_controls_when_resized(self):
        for lang in ("en", "de"):
            for original in (False, True):
                with self.subTest(lang=lang, original=original):
                    observed = []

                    def backend():
                        p = UI.active_presenter()
                        p.inventory(report={"pc": {"compatible": True, "installed": True},
                                            "frame": {"runtime_compatible": True, "installed": True,
                                                      "host": "192.168.0.25"}})
                        p.stage("startup_health")
                        observed.append(p.confirm("bridge_original_picture" if original else "bridge_picture"))
                        return 130

                    app = UI.SetupApp(backend, lang)
                    async with app.run_test(size=(100, 36)) as pilot:
                        await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
                        underlying = app.screen_stack[0]
                        brand = underlying.query_one("#brand", Static)
                        for width, height in ((100, 36), (80, 24), (60, 24), (42, 16), (100, 36)):
                            await pilot.resize_terminal(width, height)
                            await pilot.pause()
                            self.assertIs(app.screen_stack[0], underlying)
                            self.assertIs(underlying.query_one("#brand", Static), brand)
                            self.assertEqual(len(app.screen_stack), 2)
                            self.assertGreater(app.screen.styles.background.a, 0)
                            self.assertLess(app.screen.styles.background.a, 1)
                            dialog = app.screen.query_one("#confirm-dialog").region
                            self.assertGreater(dialog.x, 0)
                            self.assertGreater(dialog.y, 0)
                            self.assertLess(dialog.right, width)
                            self.assertLess(dialog.bottom, height)
                            self.assertLessEqual(abs((dialog.x + dialog.right) - width), 1)
                            self.assertLessEqual(abs((dialog.y + dialog.bottom) - height), 1)
                            for button in app.screen.query(Button):
                                self.assertTrue(dialog.contains_region(button.region), button.id)
                            self.assertEqual(app.focused.id, "picture-no")
                            if width == 100:
                                # Test actual composed output, not merely the
                                # continued existence of an obscured widget.
                                visible = " ".join(html.unescape(re.sub(r"<[^>]+>", " ", app.export_screenshot())).split())
                                self.assertIn("NATIVE PASSTHROUGH", visible)
                                self.assertIn("THIS PC" if lang == "en" else "DIESER PC", visible)
                        await pilot.press("escape")
                        await until(pilot, lambda: app.backend_finished)
                        self.assertEqual(observed, [None])
                    app.backend.join()

    async def test_picture_yes_does_not_restore_when_final_checkpoint_cleanup_fails(self):
        for lang in ("en", "de"):
            ports = PicturePorts(finish_error=OSError("owned final checkpoint unavailable"))
            app = UI.SetupApp(lambda: FLOW.Workflow(ports, UI.active_presenter()).run("update"), lang)
            async with app.run_test(size=(80, 24)) as pilot:
                await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
                await pilot.pause()
                # Exercise the actual Yes button, not a canned bool presenter.
                await pilot.click("#picture-yes")
                await until(pilot, lambda: "restore_files" in ports.events or
                            (app.prompt is not None and not isinstance(app.screen, UI.PictureScreen)))
                restored = "restore_files" in ports.events
                if restored:
                    await pilot.press("ctrl+c")
                else:
                    self.assertEqual(app.action_names, ["retry", "exit"])
                    await pilot.press("down", "enter")
                await until(pilot, lambda: app.backend_finished)
                self.assertFalse(restored, "A confirmed usable picture must never authorize automatic Undo")
                self.assertEqual(app.backend_result, 2)
                self.assertEqual(ports.events.count("finish_prepared"), 1)
                self.assertEqual(ports.events.count("prepared_health"), 1)
                self.assertNotIn("restart_original", ports.events)
                self.assertIn(app.texts["bridge_finalize_incomplete"], app.detail_history)
            app.backend.join()

    async def test_yes_finalize_retry_only_reconciles_records_without_health_or_restart(self):
        ports = PicturePorts()
        original_finish = ports.finish
        def finish(*, restored):
            original_finish(restored=restored)
            if ports.events.count("finish_prepared") == 1:
                raise OSError("owned lost finalization reply")
        ports.finish = finish
        app = UI.SetupApp(lambda: FLOW.Workflow(ports, UI.active_presenter()).run("update"), "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
            await pilot.pause()
            await pilot.press("right", "enter")
            await until(pilot, lambda: app.prompt is not None and not isinstance(app.screen, UI.PictureScreen))
            self.assertEqual(app.action_names, ["retry", "exit"])
            self.assertEqual(app.query_one(OptionList).highlighted, 0)
            await pilot.press("enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(app.backend_result, 0)
            self.assertEqual(ports.events.count("finish_prepared"), 2)
            self.assertEqual(ports.events.count("prepared_health"), 1)
            self.assertEqual(ports.events.count("restart_prepared"), 1)
            self.assertEqual(ports.events.count("prepare"), 1)
            self.assertNotIn("restore_files", ports.events)
            self.assertNotIn("restart_original", ports.events)
        app.backend.join()

    async def test_ctrl_c_during_yes_finalization_retry_keeps_current_picture_and_records(self):
        ports = PicturePorts(finish_error=OSError("owned unfinished cleanup"))
        app = UI.SetupApp(lambda: FLOW.Workflow(ports, UI.active_presenter()).run("update"), "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
            await pilot.pause()
            await pilot.press("right", "enter")
            await until(pilot, lambda: app.prompt is not None and not isinstance(app.screen, UI.PictureScreen))
            await pilot.press("ctrl+c")
            await until(pilot, lambda: app.backend_finished)
            self.assertTrue(app.stop_requested)
            self.assertEqual(app.backend_result, 130)
            self.assertEqual(ports.events.count("finish_prepared"), 1)
            self.assertNotIn("restore_files", ports.events)
            self.assertNotIn("restart_original", ports.events)
        app.backend.join()

    async def test_picture_yes_commits_once_and_ignores_stale_no_and_health_state(self):
        for lang in ("en", "de"):
            ports = PicturePorts()
            app = UI.SetupApp(lambda: FLOW.Workflow(ports, UI.active_presenter()).run("install"), lang)
            async with app.run_test(size=(80, 24)) as pilot:
                await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
                await pilot.pause()
                prompt = app.prompt
                # Health becomes stale while the user looks into the headset.
                # The positive picture verdict must not launch another check.
                ports.stale_health = True
                await pilot.press("right", "enter")
                await until(pilot, lambda: app.backend_finished)
                app.resolve_prompt(False, expected=prompt)
                self.assertEqual(app.backend_result, 0)
                self.assertEqual(ports.events.count("finish_prepared"), 1)
                self.assertEqual(ports.events.count("prepared_health"), 1)
                self.assertNotIn("restore_files", ports.events)
                self.assertEqual(app.outcome, "bridge_installed")
            app.backend.join()

    async def test_notice_failure_after_yes_commit_cannot_restore_or_repeat_finish(self):
        ports = PicturePorts()
        def backend():
            presenter = UI.active_presenter()
            real_say = presenter.say
            def say(key):
                if key == "bridge_pc_stopped":
                    raise RuntimeError("owned UI notification fixture")
                real_say(key)
            presenter.say = say
            return FLOW.Workflow(ports, presenter).run("update")
        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
            await pilot.pause()
            await pilot.press("right", "enter")
            await until(pilot, lambda: app.backend_finished or "restore_files" in ports.events)
            restored = "restore_files" in ports.events
            if not app.backend_finished:
                await pilot.press("ctrl+c")
                await until(pilot, lambda: app.backend_finished)
            self.assertFalse(restored)
            self.assertEqual(app.backend_result, 0)
            self.assertEqual(ports.events.count("finish_prepared"), 1)
            self.assertEqual(app.outcome, "bridge_installed")
        app.backend.join()

    async def test_ctrl_c_at_real_workflow_picture_neither_finalizes_nor_restores(self):
        ports = PicturePorts()
        app = UI.SetupApp(lambda: FLOW.Workflow(ports, UI.active_presenter()).run("update"), "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
            await pilot.press("ctrl+c")
            await until(pilot, lambda: app.backend_finished)
            self.assertTrue(app.stop_requested)
            self.assertEqual(app.backend_result, 130)
            self.assertNotIn("finish_prepared", ports.events)
            self.assertNotIn("restore_files", ports.events)
            self.assertNotIn("restart_original", ports.events)
        app.backend.join()

    async def test_picture_no_rechecks_actual_workflow_and_explicit_undo_requires_original_picture(self):
        class Ports:
            error = ValueError
            def __init__(self): self.events = []
            def begin(self): self.events.append("begin")
            def stop_pc(self): self.events.append("stop_pc")
            def prepare(self, action): self.events.append("prepare")
            def restart_prepared(self): self.events.append("restart_prepared")
            def prepared_health(self): self.events.append("prepared_health"); return {"software_ready": True}
            def restore_files(self): self.events.append("restore_files")
            def restart_original(self): self.events.append("restart_original")
            def original_health(self): self.events.append("original_health"); return {"software_ready": True}
            def finish(self, *, restored): self.events.append("finished_original" if restored else "finished_prepared")
            def failure(self, error, *, recovery): self.events.append("failure")

        ports = Ports()
        app = UI.SetupApp(lambda: FLOW.Workflow(ports, UI.active_presenter()).run("update"), "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
            await pilot.pause()
            self.assertEqual(app.focused.id, "picture-no")
            self.assertEqual({str(b.label) for b in app.screen.query(Button)}, {"Yes", "No", "Cancel setup"})
            await pilot.press("enter")
            await until(pilot, lambda: ports.events.count("prepared_health") == 2 and app.prompt is not None and not isinstance(app.screen, UI.PictureScreen))
            self.assertNotIn("restore_files", ports.events)
            self.assertEqual(ports.events.count("restart_prepared"), 1)
            self.assertEqual(app.query_one(OptionList).highlighted, 0)
            self.assertIn("Retry verification", str(app.query_one(OptionList).get_option_at_index(0).prompt))
            # First retry is entirely observational, with a new picture answer.
            await pilot.press("enter")
            await until(pilot, lambda: ports.events.count("prepared_health") == 3 and isinstance(app.screen, UI.PictureScreen))
            self.assertNotIn("restore_files", ports.events)
            await pilot.press("enter")  # No a second time.
            await until(pilot, lambda: ports.events.count("prepared_health") == 4 and not isinstance(app.screen, UI.PictureScreen))
            await pilot.press("down", "enter")  # Explicit Restore previous setup.
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen) and app.screen.original)
            self.assertEqual(ports.events.count("restart_original"), 1)
            self.assertNotIn("finished_original", ports.events)
            self.assertNotIn("finished_prepared", ports.events)
            await pilot.press("right", "enter")  # Yes, original picture is usable.
            await until(pilot, lambda: app.prompt is not None and not isinstance(app.screen, UI.PictureScreen))
            self.assertEqual(ports.events.count("finished_original"), 1)
            await pilot.press("down", "enter")  # Exit rather than repeating Update.
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(app.backend_result, 2)
        app.backend.join()

    async def test_picture_cancel_is_distinct_from_no_in_both_languages(self):
        for lang in ("en", "de"):
            observed = []
            def backend():
                p = UI.active_presenter()
                observed.append(p.confirm("bridge_picture"))
                return 130
            app = UI.SetupApp(backend, lang)
            async with app.run_test(size=(60, 24)) as pilot:
                await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
                await pilot.pause()
                self.assertEqual(app.focused.id, "picture-no")
                for button in app.screen.query(Button):
                    self.assertLessEqual(button.region.right, app.size.width)
                await pilot.press("left", "enter")
                await until(pilot, lambda: app.backend_finished)
                self.assertEqual(observed, [None])
                self.assertFalse(app.stop_requested)
            app.backend.join()

    async def test_arrow_navigation_and_cancel_is_default(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.choose_action(["install", "check", "exit"], recommended="check"))
            p.say("bridge_install_plan")
            observed.append(p.confirm("bridge_confirm"))
            return 0

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(100, 36)) as pilot:
            await until(pilot, lambda: app.prompt is not None)
            self.assertIs(UI.active_presenter(), app.presenter)
            self.assertEqual(app.query_one(OptionList).highlighted, 1)
            await pilot.press("up", "enter")
            await until(pilot, lambda: isinstance(app.screen, UI.ConfirmScreen))
            await pilot.pause()
            self.assertEqual(app.focused.id, "cancel")
            await pilot.press("enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, ["install", False])
            self.assertTrue(app.query_one("#complete-area").display)
            await pilot.press("enter")
        app.backend.join()
        self.assertIsNone(UI.active_presenter())

    async def test_explicit_accept_and_destructive_default_cancel(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.confirm("bridge_confirm"))
            observed.append(p.confirm_uninstall())
            return 0

        app = UI.SetupApp(backend, "de")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.ConfirmScreen))
            await pilot.pause()
            self.assertEqual(app.focused.id, "cancel")
            await pilot.press("right", "enter")
            await until(pilot, lambda: len(observed) == 1 and isinstance(app.screen, UI.ConfirmScreen))
            await pilot.pause()
            self.assertEqual(app.focused.id, "cancel")
            await pilot.press("escape")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [True, False])
            self.assertEqual(app.words["finished"], "Einrichtung abgeschlossen")
        app.backend.join()

    async def test_progress_stop_finishes_backend_cleanup_without_killing_thread(self):
        running, release, cleaned = threading.Event(), threading.Event(), threading.Event()

        def backend():
            p = UI.active_presenter()
            p.stage("frame_prepare")
            with p.progress("verify"):
                running.set()
                release.wait()
            self.assertFalse(p.confirm("bridge_picture"))
            # Ending a worker and closing its records is not a rollback.
            self.assertTrue(p.cancelled())
            cleaned.set()
            return 130

        app = UI.SetupApp(backend, "en")
        try:
            async with app.run_test(size=(80, 24)) as pilot:
                await until(pilot, running.is_set)
                self.assertTrue(app.query_one("#work").display)
                self.assertEqual(app.stage_name, "frame_prepare")
                self.assertFalse(app.backend.daemon)
                await pilot.press("ctrl+q", "ctrl+c")
                self.assertTrue(app.stop_requested)
                self.assertFalse(app.backend_finished)
                self.assertFalse(cleaned.is_set())
                release.set()
                await until(pilot, lambda: app.backend_finished)
                self.assertTrue(cleaned.is_set())
                self.assertEqual(app.backend_result, 130)
                self.assertFalse(app.query_one("#work").display)
                self.assertIn("Setup cancelled", str(app.query_one("#complete-title", Static).render()))
        finally:
            release.set()
            app.backend.join()

    async def test_error_details_and_paths_are_bounded_not_json_in_main_view(self):
        def backend():
            p = UI.active_presenter()
            p.path("C:/Users/Player/Setup/logs/setup-123.jsonl", "setup_log")
            p.path("C:/Users/Player/Setup/recovery.json", "recovery_pc")
            p.path("C:/Users/Player/Setup/lifecycle.json", "recovery_pc")
            print('{"private_report": "not ordinary UI"}')
            print("\x1b[31mError token=not-a-real-token\x1b[0m", file=sys.stderr)
            p.say("bridge_partial")
            for i in range(UI.MAX_DETAILS + 20):
                p._call(app.add_detail, "Event " + str(i))
            return 2

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(100, 40)) as pilot:
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(len(app.detail_history), UI.MAX_DETAILS)
            self.assertNotIn("private_report", str(app.query_one("#notice", Static).render()))
            self.assertIn("setup-123.jsonl", str(app.query_one("#path-list", Static).render()))
            self.assertIn("recovery.json", str(app.query_one("#path-list", Static).render()))
            self.assertIn("lifecycle.json", str(app.query_one("#path-list", Static).render()))
            self.assertFalse(app.query_one("#files", Collapsible).collapsed)
            self.assertFalse(app.query_one("#details", Collapsible).collapsed)
            await pilot.press("f2")
            self.assertTrue(app.query_one("#details", Collapsible).collapsed)
        app.backend.join()
        self.assertEqual(UI._plain("\x1b[31mpassword=secret\x1b[0m"), "password=[redacted]")

    async def test_retry_choices_keep_descriptions_and_recommended_selection(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            p.status("missing", "warning")
            observed.append(p.choose_action(["retry", "back", "exit"], recommended="retry"))
            return 0

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: app.prompt is not None)
            actions = app.query_one("#actions", OptionList)
            self.assertEqual(actions.option_count, 3)
            self.assertIn("Try again", str(actions.get_option_at_index(0).prompt))
            self.assertIn("Check the last result", str(actions.get_option_at_index(0).prompt))
            await pilot.press("down", "enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, ["back"])
        app.backend.join()

    async def test_input_modal_for_device_address_not_a_password_field(self):
        observed = []

        def backend():
            observed.append(UI.active_presenter().ask("manual"))
            return 0

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.InputScreen))
            await pilot.pause()
            field = app.screen.query_one(Input)
            self.assertFalse(field.password)
            field.value = "192.168.0.25"
            await pilot.press("enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, ["192.168.0.25"])
        app.backend.join()

    async def test_inventory_and_resize_keep_controls_accessible(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            p.inventory(report={"pc": {"compatible": True, "installed": True},
                                "frame": {"runtime_compatible": True, "host": "192.168.0.25", "installed": True}})
            p.stage("startup_health")
            observed.append(p.choose_action(["update", "uninstall", "check", "exit"], recommended="update"))
            return 0

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(100, 32)) as pilot:
            await until(pilot, lambda: app.prompt is not None)
            self.assertIn("Patch installed", str(app.query_one("#pc-status", Static).render()))
            self.assertIn("Supported version", str(app.query_one("#pc-status", Static).render()))
            self.assertIn("192.168.0.25", str(app.query_one("#frame-status", Static).render()))
            for width, height in ((80, 24), (42, 16), (120, 40)):
                await pilot.resize_terminal(width, height)
                await pilot.pause()
                self.assertLessEqual(app.query_one("#pc-card").region.right, width)
                self.assertLessEqual(app.query_one("#frame-card").region.right, width)
                self.assertEqual(app.has_class("narrow"), width < 60)
                await pilot.press("end")
            await pilot.press("enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, ["exit"])
        app.backend.join()

    async def test_ssh_suspend_uses_actual_textual_context_on_ui_thread_and_bypasses_capture(self):
        started, release = threading.Event(), threading.Event()
        driver_calls = []
        terminal = io.StringIO()

        def backend():
            started.set()
            release.wait()
            with UI.active_presenter().suspend():
                print("SSH_EXCLUSIVE_PROMPT")
                with UI.active_presenter().suspend():
                    print("SSH_NESTED_EXCLUSIVE_PROMPT")
            return 0

        app = UI.SetupApp(backend)
        with mock.patch.object(HeadlessDriver, "can_suspend", new_callable=mock.PropertyMock, return_value=True), \
             mock.patch.object(HeadlessDriver, "suspend_application_mode", lambda _: driver_calls.append(("suspend", threading.get_ident()))), \
             mock.patch.object(HeadlessDriver, "resume_application_mode", lambda _: driver_calls.append(("resume", threading.get_ident()))), \
             mock.patch("sys.__stdout__", terminal):
            try:
                async with app.run_test(size=(80, 24)) as pilot:
                    await until(pilot, started.is_set)
                    release.set()
                    await until(pilot, lambda: app.backend_finished)
                    self.assertEqual(driver_calls, [("suspend", app.ui_thread), ("resume", app.ui_thread)])
                    self.assertNotIn("SSH_EXCLUSIVE_PROMPT", "\n".join(app.detail_history))
                    self.assertIn("SSH_EXCLUSIVE_PROMPT", terminal.getvalue())
                    self.assertIn("SSH_NESTED_EXCLUSIVE_PROMPT", terminal.getvalue())
            finally:
                release.set()
                app.backend.join()

    async def test_window_close_unblocks_pending_cancel_without_killing_backend(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.confirm("bridge_confirm"))
            observed.append(p.cancelled())
            observed.append("engine cleanup")
            return 130

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.ConfirmScreen))
            await pilot.press("ctrl+c")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [False, True, "engine cleanup"])
            self.assertIn("Setup cancelled", str(app.query_one("#complete-title", Static).render()))
        app.backend.join()

    async def test_legacy_only_and_recovery_do_not_claim_new_connection_success(self):
        def backend():
            p = UI.active_presenter()
            p.inventory(frame={"runtime_compatible": True, "installed": False, "legacy_installed": True})
            p.say("bridge_rolled_back")
            return 2

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: app.backend_finished)
            state = str(app.query_one("#frame-status", Static).render())
            self.assertIn("Supported version", state)
            self.assertIn("Legacy patch only", state)
            self.assertIn("Patch not installed", state)
            self.assertIn("Previous setup restored", str(app.query_one("#complete-title", Static).render()))
            self.assertNotIn("Patch installed", str(app.query_one("#complete-title", Static).render()))
        app.backend.join()

    async def test_inline_ssh_password_is_masked_preserves_spaces_and_clears_on_submit(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            self.assertTrue(p.inline_ssh)
            observed.append(p.ssh_prompt("steamos@192.168.0.25 password:", secret=True))
            return 0

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            screen = app.screen
            field = screen.query_one(Input)
            self.assertTrue(field.password)
            self.assertEqual(field.value, "")
            self.assertEqual(field.max_length, 1024)
            field.value = " fixture-secret "
            await pilot.press("q")
            self.assertIs(app.screen, screen)
            self.assertNotIn("fixture-secret", app.export_screenshot())
            await pilot.press("enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [" fixture-secret q"])
            self.assertEqual(field.value, "")
            self.assertFalse(app.stop_requested)
            self.assertNotIn("fixture-secret", "\n".join(app.detail_history))
            self.assertNotIn("fixture-secret", str(app.query_one("#notice", Static).render()))
        app.backend.join()

    async def test_inline_ssh_wrong_password_opens_fresh_empty_prompt(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.ssh_prompt("First password:", secret=True))
            observed.append(p.ssh_prompt("Try the password again:", secret=True))
            return 0

        app = UI.SetupApp(backend, "de")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            first = app.screen
            first_field = first.query_one(Input)
            first_field.value = "fixture-wrong"
            await pilot.press("enter")
            await until(pilot, lambda: len(observed) == 1 and isinstance(app.screen, UI.SSHInputScreen) and app.screen is not first)
            await pilot.pause()
            self.assertEqual(first_field.value, "")
            second_field = app.screen.query_one(Input)
            self.assertEqual(second_field.value, "")
            second_field.value = "fixture-correct"
            await pilot.press("enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, ["fixture-wrong", "fixture-correct"])
            self.assertEqual(second_field.value, "")
            self.assertFalse(any("fixture-" in line for line in app.detail_history))
        app.backend.join()

    async def test_inline_ssh_fingerprint_requires_explicit_accept(self):
        observed = []
        fingerprint = "The authenticity of host '[frame]' cannot be established.\nED25519 key fingerprint is SHA256:AbC123+/test.\nConnect?"

        def backend():
            p = UI.active_presenter()
            observed.append(p.ssh_prompt(fingerprint, confirm=True))
            observed.append(p.ssh_prompt(fingerprint, confirm=True))
            return 0

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.ConfirmScreen))
            await pilot.pause()
            first = app.screen
            self.assertEqual(app.focused.id, "cancel")
            self.assertEqual(first.question, fingerprint)
            self.assertIn("[frame]", str(first.query_one("#confirm-question", Static).render()))
            await pilot.press("enter")
            await until(pilot, lambda: len(observed) == 1 and isinstance(app.screen, UI.ConfirmScreen) and app.screen is not first)
            await pilot.pause()
            self.assertEqual(app.focused.id, "cancel")
            await pilot.press("right", "enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [None, "yes"])
            self.assertFalse(app.stop_requested)
        app.backend.join()

    async def test_inline_ssh_escape_clears_secret_without_disabling_recovery_prompts(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.ssh_prompt("Password:", secret=True))
            observed.append(p.cancelled())
            observed.append(p.ssh_prompt("Recovery password:", secret=True))
            return 0

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            first = app.screen
            field = first.query_one(Input)
            field.value = "fixture-cancelled"
            await pilot.press("escape")
            await until(pilot, lambda: len(observed) == 2 and isinstance(app.screen, UI.SSHInputScreen) and app.screen is not first)
            await pilot.pause()
            self.assertEqual(field.value, "")
            self.assertEqual(observed, [None, False])
            await pilot.click("#ssh-cancel")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [None, False, None])
            self.assertNotIn("fixture-cancelled", "\n".join(app.detail_history))
        app.backend.join()

    async def test_only_separately_requested_recovery_clears_prior_abort(self):
        running, observed_abort, explicit_restore_request = threading.Event(), threading.Event(), threading.Event()
        observed = []

        def backend():
            p = UI.active_presenter()
            running.set()
            deadline = time.monotonic() + 5
            tick = threading.Event()
            while not p.cancelled() and time.monotonic() < deadline:
                tick.wait(.01)
            observed.append(p.cancelled())
            observed_abort.set()
            # Models a separate, explicitly selected restoration. Ctrl+C
            # alone must never call allow_recovery or start a sign-in.
            if not explicit_restore_request.wait(5):
                return 130
            p.allow_recovery()
            observed.append(p.cancelled())
            observed.append(p.ssh_prompt("Recovery password:", secret=True))
            observed.append("safe cleanup finished")
            return 130

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, running.is_set)
            await pilot.press("ctrl+c")
            await until(pilot, observed_abort.is_set)
            self.assertEqual(observed, [True])
            self.assertFalse(isinstance(app.screen, UI.SSHInputScreen))
            explicit_restore_request.set()
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            self.assertEqual(observed[:2], [True, False])
            self.assertFalse(app.backend_finished)
            self.assertIn("restoring", str(app.query_one("#notice", Static).render()))
            await pilot.press("escape")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [True, False, None, "safe cleanup finished"])
        app.backend.join()

    async def test_ctrl_q_aborts_password_prompt_and_keeps_recovery_records(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            p.path("C:/Setup/recovery.json", "recovery_pc")
            observed.append(p.ssh_prompt("Password:", secret=True))
            observed.append(p.cancelled())
            # A cancelled run cannot silently prompt for recovery credentials.
            observed.append(p.ssh_prompt("Not opened:", secret=True))
            return 130

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            field = app.screen.query_one(Input)
            field.value = "fixture-aborted"
            await pilot.press("ctrl+q")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [None, True, None])
            self.assertTrue(app.stop_requested)
            self.assertEqual(field.value, "")
            self.assertIn("Setup cancelled", str(app.query_one("#complete-title", Static).render()))
            self.assertIn("not automatically undone", str(app.query_one("#notice", Static).render()))
            self.assertIn("recovery.json", str(app.query_one("#path-list", Static).render()))
            self.assertNotIn("fixture-aborted", "\n".join(app.detail_history))
        app.backend.join()

    async def test_ctrl_c_at_picture_confirmation_publishes_abort_before_answer(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            p.say("bridge_health_ok")
            observed.append(p.confirm("bridge_picture"))
            observed.append(p.cancelled())
            return 0

        app = UI.SetupApp(backend, "de")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.PictureScreen))
            await pilot.pause()
            await pilot.press("ctrl+c")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [None, True])
            self.assertEqual(app.backend_result, 130)
            self.assertIn("Einrichtung abgebrochen", str(app.query_one("#complete-title", Static).render()))
            self.assertIn("nicht automatisch zurückgenommen", str(app.query_one("#notice", Static).render()))
            self.assertNotIn(app.words["recovering"], app.detail_history)
        app.backend.join()

    async def test_ctrl_c_aborts_action_menu_instead_of_selecting_recovery(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.choose_action(["retry", "rollback", "exit"], recommended="retry"))
            observed.append(p.cancelled())
            return 0

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: app.prompt is not None)
            await pilot.press("ctrl+c")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [None, True])
            self.assertEqual(app.backend_result, 130)
            self.assertNotIn(app.words["recovering"], app.detail_history)
        app.backend.join()

    async def test_cancelled_result_overrides_earlier_success_outcome(self):
        def backend():
            UI.active_presenter().say("bridge_installed")
            return 130

        app = UI.SetupApp(backend, "en")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: app.backend_finished)
            title = str(app.query_one("#complete-title", Static).render())
            self.assertIn("Setup cancelled", title)
            self.assertNotIn("Patch installed", title)
        app.backend.join()

    async def test_public_connection_problem_is_bounded_and_redacted(self):
        def backend():
            UI.active_presenter().problem("Own connection helper unavailable. token=fixture-never-display\x1b[31m " + "x" * 800)
            return 2

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: app.backend_finished)
            notice = app.query_one("#notice", Static)
            self.assertTrue(notice.has_class("warning"))
            self.assertIn("Own connection helper unavailable", str(notice.render()))
            self.assertIn("[redacted]", str(notice.render()))
            self.assertNotIn("fixture-never-display", str(notice.render()))
            self.assertNotIn("fixture-never-display", "\n".join(app.detail_history))
            self.assertLessEqual(len(app.detail_history[-1]), 500)
        app.backend.join()

    async def test_transport_close_cancels_only_its_ssh_prompt(self):
        observed = []

        def backend():
            p = UI.active_presenter()
            observed.append(p.confirm("bridge_confirm"))
            observed.append(p.ssh_prompt("Password:", secret=True))
            return 0

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.ConfirmScreen))
            await pilot.pause()
            self.assertFalse(await asyncio.to_thread(app.presenter.cancel_ssh_prompt))
            self.assertEqual(observed, [])
            await pilot.press("escape")
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            field = app.screen.query_one(Input)
            field.value = "fixture-closed"
            self.assertTrue(await asyncio.to_thread(app.presenter.cancel_ssh_prompt))
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [False, None])
            self.assertEqual(field.value, "")
            self.assertFalse(app.stop_requested)
            self.assertNotIn("fixture-closed", "\n".join(app.detail_history))
        app.backend.join()

    async def test_password_clipboard_shortcuts_cannot_copy_the_secret(self):
        def backend():
            UI.active_presenter().ssh_prompt("Password:", secret=True)
            return 0

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.SSHInputScreen))
            await pilot.pause()
            field = app.screen.query_one(Input)
            field.value = "fixture-private"
            field.action_select_all()
            with mock.patch.object(app, "copy_to_clipboard") as clipboard:
                field.action_copy()
                field.action_cut()
                clipboard.assert_not_called()
            await pilot.press("escape")
            await until(pilot, lambda: app.backend_finished)
        app.backend.join()

    async def test_discovery_retry_menu_can_offer_manual_and_clear_pc_only_choice(self):
        observed = []

        def backend():
            observed.append(UI.active_presenter().choose_action(["retry", "manual", "check", "exit"], recommended="retry"))
            return 0

        app = UI.SetupApp(backend, "en")
        # The discovery worker owns these shared translations. Keep this
        # isolated worker fixture independent until its commit is integrated.
        app.texts = dict(app.texts, action_manual="Enter the Frame address", action_manual_description="Use its Wi-Fi address.",
                         discovery_pc_only="Check this PC only", discovery_pc_only_description="Skip the Frame for this check.")
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: app.prompt is not None)
            choices = app.query_one("#actions", OptionList)
            self.assertIn("Check this PC only", str(choices.get_option_at_index(2).prompt))
            await pilot.press("down", "down", "enter")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, ["check"])
        app.backend.join()

    def test_plain_sanitization_and_absent_active_factory(self):
        self.assertIsNone(UI.active_presenter())
        self.assertEqual(UI._plain("[red]text\x00\x1b[4m"), "[red]text")
        self.assertEqual(len(UI._plain("x" * 10000)), UI.MAX_DETAIL_LINE)


@unittest.skipUnless(os.name == "posix", "real PTY ownership is checked on POSIX; Windows uses Pilot and installer CI")
class TextualTerminalTests(unittest.TestCase):
    def test_real_terminal_native_prompt_suspend_resume_and_restore(self):
        import pty
        import termios

        # This native console prompt is synthetic. It performs no SSH or device
        # action, receives no password and verifies the production run wrapper.
        child = r'''
import importlib.util,json,os,sys
sys.path[:]=json.loads(sys.argv[2])
spec=importlib.util.spec_from_file_location('_sfnp_setup_textual',sys.argv[1])
ui=importlib.util.module_from_spec(spec);sys.modules[spec.name]=ui;spec.loader.exec_module(ui)
phase=int(sys.argv[3]);original=ui.SetupApp.finish
def observed_finish(self,code):
 original(self,code);os.write(phase,b'FINISHED\n')
ui.SetupApp.finish=observed_finish
def backend():
 p=ui.active_presenter()
 with p.suspend():
  print('NATIVE_CONSOLE_PROMPT> ',end='',flush=True)
  answer=input()
  if answer!='console-handoff-fixture':return 3
  print('NATIVE_CONSOLE_ACK',flush=True)
 p.status('pc_ready','ok')
 return 0
code=ui.run_setup(backend,'en')
os.write(phase,('RESULT:'+str(code)+'\n').encode())
raise SystemExit(code)
'''
        master, slave = pty.openpty()
        read_phase, write_phase = os.pipe()
        before = termios.tcgetattr(slave)
        environment = dict(os.environ, TERM="xterm-256color", COLORTERM="truecolor")
        environment.pop("NO_COLOR", None)
        process = subprocess.Popen([sys.executable, "-B", "-c", child,
                                    str(ROOT / "setup/steam_link_setup_textual.py"), json.dumps(sys.path), str(write_phase)],
                                   stdin=slave, stdout=slave, stderr=slave, pass_fds=(write_phase,),
                                   start_new_session=True, env=environment)
        os.close(write_phase)
        transcript, phases = bytearray(), bytearray()
        sent_input = sent_close = False
        selector = selectors.DefaultSelector()
        selector.register(master, selectors.EVENT_READ)
        selector.register(read_phase, selectors.EVENT_READ)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and process.poll() is None:
                for key, _ in selector.select(.1):
                    try:
                        data = os.read(key.fd, 8192)
                    except OSError:
                        data = b""
                    if not data:
                        selector.unregister(key.fd)
                        continue
                    if key.fd == master:
                        transcript.extend(data)
                        if len(transcript) > 192 * 1024:
                            self.fail("terminal output exceeded its bounded fixture budget")
                    else:
                        phases.extend(data)
                if b"NATIVE_CONSOLE_PROMPT> " in transcript and not sent_input:
                    # App.suspend restored cooked, echo-enabled line input.
                    during = termios.tcgetattr(slave)
                    self.assertTrue(during[3] & termios.ICANON)
                    self.assertTrue(during[3] & termios.ECHO)
                    os.write(master, b"console-handoff-fixture\n")
                    sent_input = True
                if b"FINISHED\n" in phases and not sent_close:
                    os.write(master, b"\r")
                    sent_close = True
            self.assertEqual(process.wait(timeout=2), 0, bytes(transcript[-6000:]).decode("utf-8", "replace"))
            self.assertTrue(sent_input)
            self.assertTrue(sent_close)
            self.assertIn(b"NATIVE_CONSOLE_ACK", transcript)
            self.assertIn(b"RESULT:0", phases)
            self.assertEqual(termios.tcgetattr(slave), before)
            evidence = os.environ.get("SFNP_TEXTUAL_EVIDENCE")
            if evidence:
                destination = Path(evidence)
                destination.mkdir(parents=True, exist_ok=True)
                (destination / "native-console-pty.txt").write_bytes(transcript)
                (destination / "native-console-pty.json").write_text(json.dumps({
                    "real_posix_terminal": True, "real_textual_driver": True,
                    "synthetic_prompt": True, "credentials": False, "device_actions": False,
                    "canonical_input_restored_during_prompt": True, "terminal_state_restored_after_exit": True,
                    "exit_status": process.returncode, "transcript_bytes": len(transcript)}, indent=2) + "\n")
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            selector.close()
            for fd in (master, slave, read_phase):
                os.close(fd)


if __name__ == "__main__":
    unittest.main()
