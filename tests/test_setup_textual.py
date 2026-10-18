"""Real Textual Pilot coverage of setup presentation and engine handoff.

These tests do not connect to a device, install files or start SteamVR. They
prove UI behavior, not headset rendering or successful device installation.
"""
from __future__ import annotations

import asyncio
import importlib.util
import io
import json
import os
from pathlib import Path
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


async def until(pilot, predicate):
    for _ in range(150):
        if predicate():
            return
        await pilot.pause(.01)
    raise AssertionError("the expected UI/worker state was not reached")


class TextualSetupTests(unittest.IsolatedAsyncioTestCase):
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
            # Models the existing engine's mandatory recovery before return.
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
        finally:
            release.set()
            app.backend.join()

    async def test_error_details_and_paths_are_bounded_not_json_in_main_view(self):
        def backend():
            p = UI.active_presenter()
            p.path("C:/Users/Player/Setup/logs/setup-123.jsonl", "setup_log")
            p.path("C:/Users/Player/Setup/recovery.json", "recovery_pc")
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
            observed.append("engine cleanup")
            return 2

        app = UI.SetupApp(backend)
        async with app.run_test(size=(80, 24)) as pilot:
            await until(pilot, lambda: isinstance(app.screen, UI.ConfirmScreen))
            await pilot.press("ctrl+c")
            await until(pilot, lambda: app.backend_finished)
            self.assertEqual(observed, [False, "engine cleanup"])
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
