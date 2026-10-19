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
from textual.widgets import Button, Collapsible, Input, OptionList, Static

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


class TextualSetupTests(unittest.IsolatedAsyncioTestCase):
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
                    self.assertIn('Steam Link: '+app.texts['runtime_unknown' if key=='pc' else 'runtime_bundled'],text)
                    self.assertIn('0.2.0',text);self.assertIn('0.2.1',text)
                    self.assertTrue(body.content_region.contains_region(app.query_one('#'+key+'-status').region),key)
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

    async def test_component_versions_are_independent_in_status_and_apply_plan(self):
        for lang in ("en","de"):
            observed=[]
            pc={"compatible":True,"installed":True,"installed_version":"0.1.9","target_version":"0.2.0"}
            frame={"runtime_compatible":True,"installed":True,"installed_version":None,"target_version":"0.2.1"}
            def backend():
                p=UI.active_presenter()
                p.inventory(pc=pc,frame=frame)
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
                self.assertIn("Installed patch: Unversioned",str(app.query_one("#frame-status",Static).render()))
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
            self.assertIn("New connection installed", str(app.query_one("#pc-status", Static).render()))
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
            self.assertIn("New connection not installed", state)
            self.assertIn("Previous setup restored", str(app.query_one("#complete-title", Static).render()))
            self.assertNotIn("New connection installed", str(app.query_one("#complete-title", Static).render()))
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
            self.assertNotIn("New connection installed", title)
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
