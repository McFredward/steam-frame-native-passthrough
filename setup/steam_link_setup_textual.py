"""Textual front end for the existing synchronous, recoverable setup engine.

This module presents backend facts; it never discovers devices, changes files,
starts services or grants installation permission. A bounded authentication
broker owns OpenSSH; authentication input stays inside a private masked modal.
"""
from __future__ import annotations

import concurrent.futures
from contextlib import contextmanager, redirect_stderr, redirect_stdout
from dataclasses import dataclass
import importlib.util
from pathlib import Path
import re
import sys
import threading
from typing import Any, Callable

from rich.text import Text
from textual import events, on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Container, Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Button, Collapsible, Footer, Input, Label, LoadingIndicator, OptionList, RichLog, Static
from textual.widgets.option_list import Option


_ACTIVE: SetupApp | None = None
_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_SECRET = re.compile(r"(?i)(password|passphrase|pairing[_ -]?key|secret|token)\s*[:=]\s*\S+")
MAX_DETAILS = 160
MAX_DETAIL_LINE = 1200

WORDS = {
    "en": {
        "subtitle": "Native camera passthrough for Steam Link",
        "checking": "Not checked yet", "pc": "THIS PC", "frame": "STEAM FRAME",
        "ready": "Supported version", "attention": "Needs attention", "connected": "Connected",
        "absent": "Not connected", "installed": "Installed", "not_installed": "Not installed",
        "connection_installed": "New connection installed", "connection_missing": "New connection not installed", "legacy_only": "Legacy patch only",
        "restored": "Previous setup restored", "removed": "Connection removed", "current": "Already up to date", "check_done": "Check complete · no changes",
        "working": "Preparing your setup", "review": "Choose what happens next",
        "details": "Details & activity", "files": "Logs & recovery files", "empty_files": "Paths will appear here when they are created.",
        "choose": "Use ↑ ↓ to explore, then Enter to select. Nothing changes before confirmation.",
        "cancel": "Cancel", "continue": "Continue", "close": "Close setup", "confirm": "Before we continue",
        "finished": "Setup finished", "failed": "Setup needs your attention", "finish_help": "You can now close this window. Your log and recovery paths remain available below.",
        "stop_pending": "Stop requested. The current safe step will finish first; any required recovery stays under setup control.",
        "stop_help": "Setup is still working. It will not close while a device operation or recovery is running.",
        "select": "Select", "stop": "Stop / back", "detail_key": "Details", "paths_key": "Log paths",
        "ssh": "Connect to your Steam Frame", "ssh_help": "Sign in below to connect securely. Your password is hidden and is never written to the setup log.",
        "ssh_secret_help": "Your input stays hidden and is never written to the setup log.",
        "ssh_confirm_help": "Check this device identity before continuing. Cancel leaves this connection unapproved.",
        "ssh_input_help": "This response is sent only to OpenSSH for the current connection.",
        "ssh_accept": "Trust and connect", "ssh_submit": "Connect", "ssh_placeholder": "Enter your password",
        "recovering": "Completing the safe step or restoring the previous setup. You can sign in again if recovery needs a connection.",
        "input_help": "Leave empty to continue without this device. Setup asks for a password separately when needed.",
        "complete": "Complete", "pc_inventory": "Check this PC", "frame_discovery": "Find your Frame", "frame_inventory": "Check your Frame",
        "frame_prepare": "Prepare the Frame", "pc_install": "Prepare this PC", "frame_commit": "Apply the prepared changes",
        "runtime_stop": "Close PC SteamVR", "runtime_start": "Restart the Frame interface", "startup_health": "Check the Frame startup",
        "rollback": "Restore the previous setup", "safe": "Changes always require your confirmation",
        "small": "Small terminal · scroll to see every step", "notice": "Latest update", "activity_empty": "A short activity history will appear here.",
    },
    "de": {
        "subtitle": "Natives Kamera-Passthrough über Steam Link",
        "checking": "Noch nicht geprüft", "pc": "DIESER PC", "frame": "STEAM FRAME",
        "ready": "Unterstützte Version", "attention": "Bitte prüfen", "connected": "Verbunden",
        "absent": "Nicht verbunden", "installed": "Installiert", "not_installed": "Nicht installiert",
        "connection_installed": "Neue Verbindung installiert", "connection_missing": "Neue Verbindung nicht installiert", "legacy_only": "Nur der bisherige Patch",
        "restored": "Vorheriger Zustand wiederhergestellt", "removed": "Verbindung entfernt", "current": "Bereits aktuell", "check_done": "Prüfung fertig · nichts geändert",
        "working": "Die Einrichtung wird vorbereitet", "review": "Wähle den nächsten Schritt",
        "details": "Details & Verlauf", "files": "Logs & Wiederherstellungsdateien", "empty_files": "Hier erscheinen die Pfade, sobald sie angelegt wurden.",
        "choose": "Mit ↑ ↓ auswählen und mit Enter öffnen. Änderungen werden erst nach deiner Bestätigung vorgenommen.",
        "cancel": "Abbrechen", "continue": "Weiter", "close": "Einrichtung schließen", "confirm": "Bevor es weitergeht",
        "finished": "Einrichtung abgeschlossen", "failed": "Die Einrichtung benötigt deine Hilfe", "finish_help": "Du kannst dieses Fenster jetzt schließen. Log und Wiederherstellungspfade bleiben unten verfügbar.",
        "stop_pending": "Abbruch angefordert. Der aktuelle sichere Schritt wird zuerst beendet; eine nötige Wiederherstellung wird weiter überwacht.",
        "stop_help": "Die Einrichtung arbeitet noch. Während einer Geräteaktion oder Wiederherstellung wird sie nicht geschlossen.",
        "select": "Auswählen", "stop": "Stopp / zurück", "detail_key": "Details", "paths_key": "Log-Pfade",
        "ssh": "Mit deiner Steam Frame verbinden", "ssh_help": "Melde dich unten sicher an. Dein Passwort bleibt verborgen und wird niemals in das Setup-Log geschrieben.",
        "ssh_secret_help": "Deine Eingabe bleibt verborgen und wird niemals in das Setup-Log geschrieben.",
        "ssh_confirm_help": "Prüfe diese Gerätekennung, bevor du fortfährst. Abbrechen lässt diese Verbindung unbestätigt.",
        "ssh_input_help": "Diese Antwort wird nur für die aktuelle Verbindung an OpenSSH übergeben.",
        "ssh_accept": "Vertrauen und verbinden", "ssh_submit": "Verbinden", "ssh_placeholder": "Passwort eingeben",
        "recovering": "Der sichere Schritt wird abgeschlossen oder der vorherige Zustand wiederhergestellt. Du kannst dich erneut anmelden, wenn dafür eine Verbindung nötig ist.",
        "input_help": "Leer lassen, um ohne dieses Gerät fortzufahren. Ein Passwort wird bei Bedarf separat abgefragt.",
        "complete": "Fertig", "pc_inventory": "Diesen PC prüfen", "frame_discovery": "Deine Frame finden", "frame_inventory": "Deine Frame prüfen",
        "frame_prepare": "Frame vorbereiten", "pc_install": "Diesen PC vorbereiten", "frame_commit": "Vorbereitete Änderungen anwenden",
        "runtime_stop": "PC-SteamVR schließen", "runtime_start": "Frame-Oberfläche neu starten", "startup_health": "Frame-Start prüfen",
        "rollback": "Vorherigen Zustand wiederherstellen", "safe": "Änderungen erfordern immer deine Bestätigung",
        "small": "Kleines Terminal · für weitere Schritte scrollen", "notice": "Aktueller Hinweis", "activity_empty": "Hier erscheint ein kurzer Verlauf.",
    },
}


def _plain(value: Any, limit: int = MAX_DETAIL_LINE) -> str:
    value = _ANSI.sub("", str(value))
    value = "".join(c for c in value if c.isprintable() or c in "\n\t")
    return _SECRET.sub(r"\1=[redacted]", value)[:limit]


def _existing_words():
    module = sys.modules.get("_sfnp_setup_ui")
    if module is None:
        name = "_sfnp_textual_words"
        module = sys.modules.get(name)
        if module is None:
            spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name("steam_link_setup_ui.py"))
            module = importlib.util.module_from_spec(spec)
            sys.modules[name] = module
            spec.loader.exec_module(module)
    return module


@dataclass
class Prompt:
    future: concurrent.futures.Future
    cancel: Any = None
    kind: str = "general"


class ConfirmScreen(ModalScreen[bool]):
    """An explicit, keyboard-accessible confirmation; Cancel has initial focus."""
    BINDINGS = [Binding("escape", "cancel", show=False), Binding("ctrl+c", "cancel", show=False),
                Binding("left,up", "app.focus_previous", show=False), Binding("right,down", "app.focus_next", show=False)]

    def __init__(self, title: str, question: str, plan: str, cancel: str, accept: str, destructive: bool = False):
        super().__init__()
        self.heading, self.question, self.plan = title, question, plan
        self.cancel_label, self.accept_label, self.destructive = cancel, accept, destructive

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-dialog"):
            yield Static(self.heading, id="confirm-title", markup=False)
            with VerticalScroll(id="confirm-copy"):
                if self.plan:
                    yield Static(self.plan, classes="plan-copy", markup=False)
                yield Static(self.question, id="confirm-question", markup=False)
            with Horizontal(id="confirm-buttons"):
                yield Button(self.cancel_label, id="cancel", variant="default")
                yield Button(self.accept_label, id="accept", variant="error" if self.destructive else "primary")

    def on_mount(self):
        self.query_one("#cancel", Button).focus()

    @on(Button.Pressed)
    def answer(self, event: Button.Pressed):
        self.dismiss(event.button.id == "accept")

    def action_cancel(self):
        self.dismiss(False)


class InputScreen(ModalScreen[str]):
    BINDINGS = [Binding("escape", "cancel", show=False), Binding("ctrl+c", "cancel", show=False)]

    def __init__(self, question: str, words: dict):
        super().__init__()
        self.question, self.words = question, words

    def compose(self) -> ComposeResult:
        with Vertical(id="input-dialog"):
            yield Static(self.question, id="input-title", markup=False)
            yield Input(placeholder="192.168.0.25", id="setup-input")
            yield Static(self.words["input_help"], classes="muted", markup=False)
            with Horizontal(id="input-buttons"):
                yield Button(self.words["cancel"], id="input-cancel")
                yield Button(self.words["continue"], id="input-continue", variant="primary")

    def on_mount(self):
        self.query_one(Input).focus()

    @on(Input.Submitted)
    def submitted(self):
        self.dismiss(self.query_one(Input).value.strip())

    @on(Button.Pressed)
    def answer(self, event: Button.Pressed):
        self.dismiss(self.query_one(Input).value.strip() if event.button.id == "input-continue" else "")

    def action_cancel(self):
        self.dismiss("")


class _PrivateSSHInput(Input):
    """A password cannot become clipboard content through copy/cut shortcuts."""
    def action_copy(self):
        if not self.password:
            super().action_copy()

    def action_cut(self):
        if self.password:
            self.delete_selection()
        else:
            super().action_cut()


class SSHInputScreen(ModalScreen[str | None]):
    """Private OpenSSH response input; values never enter notices or history."""
    BINDINGS = [Binding("escape", "cancel", show=False), Binding("ctrl+c", "cancel", show=False)]

    def __init__(self, question: str, words: dict, *, secret: bool):
        super().__init__()
        self.question, self.words, self.secret = question, words, secret

    def compose(self) -> ComposeResult:
        with Vertical(id="ssh-dialog"):
            yield Static(self.words["ssh"], id="ssh-title", markup=False)
            yield Static(self.question, id="ssh-question", markup=False)
            yield _PrivateSSHInput(password=self.secret, placeholder=self.words["ssh_placeholder"] if self.secret else "",
                                  max_length=1024, validate_on=[], id="ssh-response")
            yield Static(self.words["ssh_secret_help" if self.secret else "ssh_input_help"], classes="muted", markup=False)
            with Horizontal(id="ssh-buttons"):
                yield Button(self.words["cancel"], id="ssh-cancel")
                yield Button(self.words["ssh_submit"], id="ssh-submit", variant="primary")

    def on_mount(self):
        self.query_one(Input).focus()

    def clear_response(self):
        for field in self.query(Input):
            field.value = ""

    def dismiss(self, result=None):
        self.clear_response()
        return super().dismiss(result)

    @on(Input.Changed)
    def changed(self, event: Input.Changed):
        event.stop()

    @on(Input.Submitted)
    def submitted(self, event: Input.Submitted):
        event.stop()
        # Preserve spaces in passwords; the broker validates the bounded value.
        self.dismiss(self.query_one(Input).value)

    @on(Button.Pressed)
    def answer(self, event: Button.Pressed):
        self.dismiss(self.query_one(Input).value if event.button.id == "ssh-submit" else None)

    def action_cancel(self):
        self.dismiss(None)

    def on_unmount(self):
        self.clear_response()


class _Capture:
    """Bounded presentation-only console capture; suspended SSH bypasses it."""
    encoding = "utf-8"

    def __init__(self, app: SetupApp, original):
        self.app, self.original, self.pending = app, original, ""

    def write(self, value):
        count = len(value)
        if not self.app.ui_available:
            sys.__stderr__.write(_plain(value))
            return count
        self.pending = (self.pending + str(value))[-8192:]
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            if line.strip():
                self._deliver(line)
        return count

    def flush(self):
        if self.pending:
            pending, self.pending = self.pending, ""
            if self.app.ui_available:
                self._deliver(pending)

    def _deliver(self, value):
        if threading.get_ident() == self.app.ui_thread:
            self.app.add_detail(_plain(value))
        else:
            self.app.call_from_thread(self.app.add_detail, _plain(value))

    def isatty(self):
        return True

    def fileno(self):
        return self.original.fileno()


class TextualPresenter:
    prompting = True
    interactive = True
    inline_ssh = True

    def __init__(self, app: SetupApp):
        self.app, self.lang = app, app.lang
        self._pause_depth = 0
        self._suspension = None
        self._tasks: list[str] = []

    def text(self, key):
        return self.app.texts[key]

    def _call(self, callback, *args, **kwargs):
        if not self.app.ui_available:
            return None
        if threading.get_ident() == self.app.ui_thread:
            return callback(*args, **kwargs)
        return self.app.call_from_thread(callback, *args, **kwargs)

    def say(self, key):
        self._call(self.app.show_notice, self.text(key), key)

    def status(self, key, state="info"):
        if state not in ("info", "ok", "warning"):
            raise ValueError("unknown status state")
        self._call(self.app.show_notice, self.text(key), key, state)

    def path(self, value, key="report"):
        self._call(self.app.add_path, self.text(key).strip(), _plain(value, 4096))

    def banner(self, inspection=True):
        self.say("inspection" if inspection else "setup")

    def section(self, key):
        self._call(self.app.set_heading, self.text(key))

    def stage(self, key):
        self._call(self.app.set_stage, key)

    def inventory(self, *, pc=None, frame=None, report=None):
        self._call(self.app.update_inventory, pc, frame, report)

    @contextmanager
    def progress(self, key):
        self._tasks.append(key)
        self._call(self.app.set_working, self.text(key))
        okay = False
        try:
            yield
            okay = True
        finally:
            self._tasks.pop()
            self._call(self.app.add_detail, ("✓ " if okay else "! ") + self.text(key))
            self._call(self.app.set_working, self.text(self._tasks[-1]) if self._tasks else None)

    @contextmanager
    def suspend(self):
        """Restore the actual terminal only for a real OpenSSH interaction.

        Driver access happens on the UI thread. The backend owns the blocking
        SSH process; no password or terminal input passes through this app.
        """
        outer = self._pause_depth == 0
        self._pause_depth += 1
        try:
            if outer:
                self._call(self.app.show_notice, self.app.words["ssh_help"], "ssh")
                self._suspension = self.app.suspend()
                self._call(self._suspension.__enter__)
            yield
        finally:
            self._pause_depth -= 1
            if outer and self._suspension is not None:
                suspension, self._suspension = self._suspension, None
                self._call(suspension.__exit__, None, None, None)

    def _wait(self, render, cancel, *, kind="general"):
        if self.app.stop_requested or not self.app.ui_available:
            return cancel
        prompt = Prompt(concurrent.futures.Future(), cancel, kind)
        self._call(self.app.start_prompt, prompt, render)
        if not self.app.ui_available and not prompt.future.done():
            return cancel
        return prompt.future.result()

    def ssh_prompt(self, prompt, *, secret=False, confirm=False):
        """Present a broker-validated SSH prompt without logging its response."""
        if (type(prompt) is not str or not 0 < len(prompt) <= 4096
                or type(secret) is not bool or type(confirm) is not bool or (secret and confirm)):
            raise ValueError("SSH prompt options are invalid")
        question = _plain(prompt, 4096)
        if confirm:
            return self._wait(lambda request: self.app.push_screen(ConfirmScreen(
                self.app.words["ssh"], question, self.app.words["ssh_confirm_help"],
                self.app.words["cancel"], self.app.words["ssh_accept"]),
                lambda value: self.app.resolve_prompt("yes" if value else None, expected=request)), None, kind="ssh")
        return self._wait(lambda request: self.app.push_screen(SSHInputScreen(
            question, self.app.words, secret=secret),
            lambda value: self.app.resolve_prompt(value, expected=request)), None, kind="ssh")

    def cancel_ssh_prompt(self):
        """Unblock only an SSH prompt after its transport has closed."""
        return bool(self._call(self.app.cancel_ssh_prompt))

    def cancelled(self):
        return self.app.stop_requested or not self.app.ui_available

    def allow_recovery(self):
        """The backend calls this only when authorized recovery begins."""
        self._call(self.app.allow_recovery)

    def ask(self, key):
        if key == "done_prompt":
            return ""
        return self._wait(lambda prompt: self.app.push_screen(InputScreen(self.text(key), self.app.words),
                           lambda value: self.app.resolve_prompt(value, expected=prompt)), "")

    def choose_action(self, actions, recommended=None):
        known = ("install", "update", "repair", "uninstall", "check", "exit", "retry", "back", "rollback", "continue", "manual")
        if (type(actions) not in (list, tuple) or len(actions) > len(known)
                or any(type(a) is not str or a not in known for a in actions)
                or len(set(actions)) != len(actions)):
            raise ValueError("actions must be distinct supported action names")
        if not actions:
            return None
        return self._wait(lambda prompt: self.app.show_actions(actions, recommended), None)

    def confirm(self, key):
        plan = self.app.plan
        if key in ("bridge_picture", "bridge_started"):
            plan = self.text("bridge_health_ok")
        elif key in ("bridge_recover_confirm", "bridge_continue"):
            plan = self.text("bridge_stop_first")
        return self._wait(lambda prompt: self.app.push_screen(ConfirmScreen(
            self.app.words["confirm"], self.text(key), plan,
            self.app.words["cancel"], self.app.words["continue"]),
            lambda value: self.app.resolve_prompt(value, expected=prompt)), False)

    def confirm_uninstall(self):
        return self._wait(lambda prompt: self.app.push_screen(ConfirmScreen(
            self.app.words["confirm"], self.text("confirm_uninstall"), self.app.plan,
            self.app.words["cancel"], self.text("confirm_remove"), True),
            lambda value: self.app.resolve_prompt(value, expected=prompt)), False)

    def close(self):
        # The engine closes its presenter before its worker has returned.
        # Only the app's completion path can expose the final Close button.
        pass


class SetupApp(App[int]):
    """One main UI thread and one joined backend thread, with safe prompt handoff."""
    TITLE = "Steam Link Native Passthrough"
    ENABLE_COMMAND_PALETTE = False
    BINDINGS = [Binding("ctrl+c,ctrl+q", "request_stop", "Stop / back", priority=True),
                Binding("q,escape", "request_stop", show=False),
                Binding("f2", "details", "Details"), Binding("f3", "paths", "Log paths")]
    CSS = """
    Screen { background: #0b111b; color: #dbe7f3; }
    #masthead { height: 5; padding: 1 3 0 3; background: #101e2e; border-bottom: solid #20465c; }
    #brand { color: #8de2ec; text-style: bold; }
    #tagline { color: #8ea9bc; }
    #body { padding: 1 3; }
    #devices { height: auto; min-height: 5; margin-bottom: 1; }
    .device { width: 1fr; height: auto; min-height: 5; padding: 0 2; border: round #234258; background: #101c2a; }
    #pc-card { margin-right: 1; }
    .device-name { color: #74cbd9; text-style: bold; }
    .device-status { color: #c8d6e5; }
    #step { height: auto; min-height: 1; color: #79d6e6; margin-bottom: 1; text-style: bold; }
    #work { height: auto; min-height: 1; }
    #spinner { width: 5; height: 1; color: #79d6e6; }
    #work-label { width: 1fr; height: auto; }
    #notice { height: auto; padding: 1 2; border-left: thick #3c87a4; background: #112233; margin-bottom: 1; }
    #notice.warning { border-left: thick #e1aa69; background: #30261e; }
    #notice.ok { border-left: thick #75cba8; }
    #action-area { height: auto; }
    #action-heading { height: auto; text-style: bold; margin-bottom: 1; }
    #actions { height: auto; max-height: 19; border: round #315a72; background: #101c2a; padding: 0 1; }
    #actions > .option-list--option-highlighted { background: #164c66; color: #ffffff; }
    #action-help { height: auto; color: #8ea9bc; margin: 1 0; }
    #complete-area { height: auto; }
    #complete-title { color: #8de2ec; text-style: bold; margin: 1 0; }
    #close { width: auto; min-width: 20; margin: 1 0; }
    Collapsible { background: #0e1926; border: none; padding: 0; margin-top: 1; }
    Collapsible > Contents { padding: 0 1; }
    #details-log { height: 10; min-height: 4; background: #0b111b; border: round #203e52; }
    #path-list { height: auto; color: #a8bfce; }
    #small { height: auto; color: #dbb67d; margin-bottom: 1; }
    .muted { color: #8ea9bc; height: auto; }
    Footer { background: #101e2e; color: #a5bdce; }
    Footer > .footer--key { background: #19394b; color: #8de2ec; }
    SetupApp.compact #masthead { height: 4; padding: 0 1; }
    SetupApp.compact #body { padding: 0 1; }
    SetupApp.compact #notice { padding: 0 1; }
    SetupApp.compact .device { padding: 0 1; min-height: 4; }
    SetupApp.compact #devices { min-height: 4; }
    SetupApp.narrow #devices { layout: vertical; }
    SetupApp.narrow .device { width: 1fr; margin: 0; }
    ConfirmScreen, InputScreen, SSHInputScreen { align: center middle; background: #030811cc; }
    #confirm-dialog, #input-dialog, #ssh-dialog { width: 70; max-width: 94%; height: auto; max-height: 90%; padding: 1 2; border: round #61bfd4; background: #122336; }
    #confirm-title, #input-title, #ssh-title { color: #8de2ec; text-style: bold; height: auto; margin-bottom: 1; }
    #ssh-question { height: auto; margin-bottom: 1; }
    #confirm-copy { height: auto; max-height: 16; }
    #confirm-question { height: auto; text-style: bold; margin: 1 0; }
    .plan-copy { height: auto; color: #bdd0e1; }
    #confirm-buttons, #input-buttons, #ssh-buttons { height: auto; align-horizontal: right; margin-top: 1; }
    Button { margin-left: 1; }
    Button:focus { text-style: bold; }
    #setup-input, #ssh-response { margin-bottom: 1; }
    """

    def __init__(self, callback: Callable[[], int], lang=None):
        super().__init__()
        existing = _existing_words()
        self.lang = existing.language(lang)
        self.texts, self.words = existing.TEXT[self.lang], WORDS[self.lang]
        self.callback = callback
        self.presenter = TextualPresenter(self)
        self.ui_thread = threading.get_ident()
        self.backend: threading.Thread | None = None
        self.backend_result = 2
        self.backend_finished = False
        self.ui_available = False
        self.stop_requested = False
        self.prompt: Prompt | None = None
        self.plan = ""
        self.outcome = None
        self.stage_name = "pc_inventory"
        self.paths: dict[tuple[str, str], None] = {}
        self.detail_history: list[str] = []
        self.action_names: list[str] = []
        self.set_bindings()

    def set_bindings(self):
        # App.bind() recreates bindings with priority=False. Preserve priority
        # for stop keys even when a focused widget consumes ordinary keys.
        self._bindings.bind("ctrl+c,ctrl+q", "request_stop", description=self.words["stop"], show=True, priority=True)
        self.bind("q,escape", "request_stop", show=False)
        self.bind("f2", "details", description=self.words["detail_key"], show=True)
        self.bind("f3", "paths", description=self.words["paths_key"], show=True)

    def compose(self) -> ComposeResult:
        with Vertical(id="masthead"):
            yield Static("◉  STEAM LINK  /  NATIVE PASSTHROUGH", id="brand", markup=False)
            yield Static(self.words["subtitle"], id="tagline", markup=False)
        with VerticalScroll(id="body"):
            yield Static(self.words["small"], id="small", markup=False)
            with Horizontal(id="devices"):
                with Vertical(id="pc-card", classes="device"):
                    yield Static(self.words["pc"], classes="device-name", markup=False)
                    yield Static(self.words["checking"], id="pc-status", classes="device-status", markup=False)
                with Vertical(id="frame-card", classes="device"):
                    yield Static(self.words["frame"], classes="device-name", markup=False)
                    yield Static(self.words["checking"], id="frame-status", classes="device-status", markup=False)
            yield Static(self.words["pc_inventory"], id="step", markup=False)
            with Horizontal(id="work"):
                yield LoadingIndicator(id="spinner")
                yield Static(self.words["working"], id="work-label", markup=False)
            yield Static(self.texts["setup"], id="notice", markup=False)
            with Vertical(id="action-area"):
                yield Static(self.words["review"], id="action-heading", markup=False)
                yield OptionList(id="actions")
                yield Static(self.words["choose"], id="action-help", markup=False)
            with Vertical(id="complete-area"):
                yield Static("", id="complete-title", markup=False)
                yield Static(self.words["finish_help"], classes="muted", markup=False)
                yield Button(self.words["close"], id="close", variant="primary")
            with Collapsible(title=self.words["files"], collapsed=True, id="files"):
                yield Static(self.words["empty_files"], id="path-list", markup=False)
            with Collapsible(title=self.words["details"], collapsed=True, id="details"):
                yield RichLog(id="details-log", max_lines=MAX_DETAILS, wrap=True, markup=False, highlight=False)
        yield Footer()

    def on_mount(self):
        global _ACTIVE
        if _ACTIVE is not None and _ACTIVE is not self:
            raise RuntimeError("a setup interface is already active")
        _ACTIVE = self
        self.ui_available = True
        self.ui_thread = threading.get_ident()
        self.query_one("#action-area").display = False
        self.query_one("#complete-area").display = False
        self._resize(self.size.width, self.size.height)
        self.backend = threading.Thread(target=self._run_backend, name="sfnp-setup-engine", daemon=False)
        self.backend.start()

    def _run_backend(self):
        stdout, stderr = _Capture(self, sys.stdout), _Capture(self, sys.stderr)
        try:
            with redirect_stdout(stdout), redirect_stderr(stderr):
                result = self.callback()
                stdout.flush()
                stderr.flush()
            code = int(result or 0)
        except BaseException as error:
            # The engine owns recovery. Never force-cancel or kill its thread.
            code = 130 if isinstance(error, KeyboardInterrupt) else 2
            if self.ui_available:
                self.call_from_thread(self.add_detail, type(error).__name__ + ": " + _plain(error))
        self.backend_result = code
        if self.ui_available:
            self.call_from_thread(self.finish, code)

    def on_resize(self, event: events.Resize):
        self._resize(event.size.width, event.size.height)

    def _resize(self, width, height):
        self.set_class(width < 95 or height < 30, "compact")
        self.set_class(width < 60, "narrow")
        if self.is_mounted:
            self.query_one("#small").display = width < 60 or height < 22

    def set_heading(self, value):
        self.query_one("#action-heading", Static).update(_plain(value))

    def set_stage(self, key):
        self.stage_name = key
        label = self.words.get(key, self.words["working"])
        self.query_one("#step", Static).update(label)
        if key == "complete":
            self.set_working(None)
        elif self.prompt is None:
            self.set_working(label)

    def set_working(self, label):
        self.query_one("#work").display = label is not None
        if label is not None:
            self.query_one("#work-label", Static).update(_plain(label))

    def show_notice(self, value, key="", state="info"):
        notice = self.query_one("#notice", Static)
        notice.update(_plain(value, 4096))
        notice.set_class(state == "warning" or key in ("bridge_partial", "bridge_recovery_incomplete", "missing"), "warning")
        notice.set_class(state == "ok", "ok")
        self.add_detail(value)
        if key in ("bridge_installed", "bridge_removed", "bridge_rolled_back", "bridge_current", "check_finish"):
            self.outcome = key
        if key.endswith("_plan") or key in ("bridge_stop_first", "bridge_retry_ambiguous"):
            self.plan = value
        if key in ("pc_ready", "pc_partial"):
            self.query_one("#pc-status", Static).update(self.words["ready" if key == "pc_ready" else "attention"])
        if key in ("found", "receiver_ready", "receiver_changed", "pc_only"):
            self.query_one("#frame-status", Static).update(self.words[{"found": "connected", "receiver_ready": "ready", "receiver_changed": "attention", "pc_only": "absent"}[key]])

    def update_inventory(self, pc=None, frame=None, report=None):
        if isinstance(report, dict):
            pc = report.get("pc", pc)
            frame = report.get("frame", frame)
        for key, value in (("pc", pc), ("frame", frame)):
            if not isinstance(value, dict):
                continue
            files = value.get("files")
            ready = value.get("runtime_compatible", value.get("compatible")) is True
            if isinstance(files, dict) and files:
                ready = all(isinstance(item, dict) and item.get("status") == "match" for item in files.values())
            state = self.words["ready"] if ready else self.words["attention"]
            installed = value.get("installed")
            if isinstance(installed, bool):
                if key == "frame" and not installed and value.get("legacy_installed") is True:
                    state += "\n" + self.words["legacy_only"]
                state += "\n" + self.words["connection_installed" if installed else "connection_missing"]
            host = value.get("host") if key == "frame" else None
            if isinstance(host, str):
                state += "\n" + _plain(host, 100)
            self.query_one("#" + key + "-status", Static).update(state)

    def add_detail(self, value):
        value = _plain(value)
        self.detail_history.append(value)
        self.detail_history = self.detail_history[-MAX_DETAILS:]
        self.query_one("#details-log", RichLog).write(Text(value))

    def add_path(self, label, value):
        self.paths[(_plain(label, 160), _plain(value, 4096))] = None
        if len(self.paths) > 32:
            del self.paths[next(iter(self.paths))]
        self.query_one("#path-list", Static).update("\n\n".join(label + "\n" + path for label, path in self.paths))
        # Paths stay visible after a failure without burying the normal flow.
        if "log" in label.casefold():
            self.query_one("#files", Collapsible).collapsed = False

    def start_prompt(self, prompt, render):
        if self.stop_requested:
            prompt.future.set_result(prompt.cancel)
            return
        self.prompt = prompt
        self.set_working(None)
        render(prompt)

    def resolve_prompt(self, answer, *, expected=None):
        # A dismissed authentication screen may finish after a retry has
        # opened a new prompt. Its callback must not answer the new request.
        if expected is not None and self.prompt is not expected:
            return
        prompt, self.prompt = self.prompt, None
        self.query_one("#action-area").display = False
        if prompt is not None and not prompt.future.done():
            prompt.future.set_result(answer)

    def cancel_ssh_prompt(self):
        prompt = self.prompt
        if prompt is None or prompt.kind != "ssh":
            return False
        if isinstance(self.screen, (SSHInputScreen, ConfirmScreen)):
            self.screen.action_cancel()
        self.resolve_prompt(None, expected=prompt)
        return True

    def allow_recovery(self):
        self.stop_requested = False
        self.show_notice(self.words["recovering"], state="warning")
        self.set_working(self.words["rollback"])

    def show_actions(self, actions, recommended):
        self.action_names = list(actions)
        self.query_one("#action-area").display = True
        self.set_heading(self.words["review"])
        choices = self.query_one("#actions", OptionList)
        choices.clear_options()
        for action in actions:
            discovery_pc_only = action == "check" and all(a in actions for a in ("retry", "manual", "exit"))
            label = self.texts.get("discovery_pc_only", self.texts["action_check"]) if discovery_pc_only else self.texts["action_" + action]
            description = (self.texts.get("discovery_pc_only_description", self.texts["action_check_description"])
                           if discovery_pc_only else self.texts["action_" + action + "_description"])
            suffix = "  · " + self.texts["recommended"] if action == recommended else ""
            text = Text(label + suffix + "\n", style="bold #dcecf5")
            text.append(description, style="not bold #93adbf")
            choices.add_option(Option(text, id=action))
        choices.highlighted = actions.index(recommended) if recommended in actions else 0
        choices.focus()
        choices.scroll_visible()

    @on(OptionList.OptionSelected, "#actions")
    def action_selected(self, event: OptionList.OptionSelected):
        self.resolve_prompt(event.option.id)

    def finish(self, code):
        self.backend_result = code
        self.backend_finished = True
        self.query_one("#action-area").display = False
        self.query_one("#work").display = False
        self.query_one("#complete-area").display = True
        self.set_stage("complete")
        outcome = {"bridge_installed": "connection_installed", "bridge_removed": "removed",
                   "bridge_rolled_back": "restored", "bridge_current": "current", "check_finish": "check_done"}.get(self.outcome)
        self.query_one("#complete-title", Static).update(self.words[outcome or ("finished" if code == 0 else "failed")])
        if code != 0:
            self.query_one("#files", Collapsible).collapsed = False
            self.query_one("#details", Collapsible).collapsed = False
        self.query_one("#close", Button).focus()
        self.query_one("#complete-area").scroll_visible()

    @on(Button.Pressed, "#close")
    def close_pressed(self):
        if self.backend_finished:
            self.exit(self.backend_result)

    def action_request_stop(self):
        if self.backend_finished:
            self.exit(self.backend_result)
            return
        if isinstance(self.screen, (ConfirmScreen, InputScreen, SSHInputScreen)):
            self.screen.action_cancel()
            return
        if self.prompt is not None:
            self.resolve_prompt(self.prompt.cancel)
            return
        self.stop_requested = True
        self.show_notice(self.words["stop_pending"], state="warning")

    def action_quit(self):
        # Textual's inherited Ctrl+Q must obey the same backend cleanup contract.
        self.action_request_stop()

    def action_details(self):
        details = self.query_one("#details", Collapsible)
        details.collapsed = not details.collapsed
        details.scroll_visible()

    def action_paths(self):
        files = self.query_one("#files", Collapsible)
        files.collapsed = not files.collapsed
        files.scroll_visible()

    def on_unmount(self):
        global _ACTIVE
        self.ui_available = False
        self.stop_requested = True
        if self.prompt is not None and not self.prompt.future.done():
            self.prompt.future.set_result(self.prompt.cancel)
        if _ACTIVE is self:
            _ACTIVE = None


def active_presenter(**_kwargs):
    """The existing Presenter factory uses this only during an active app."""
    return _ACTIVE.presenter if _ACTIVE is not None else None


def run_setup(callback: Callable[[], int], lang=None) -> int:
    """Run an interactive engine with Textual; return only after its cleanup.

    The callback must keep its existing recovery paths. Closing the UI never
    kills a backend thread, and no daemon or timeout can abandon a device.
    """
    app = SetupApp(callback, lang)
    try:
        result = app.run()
        return app.backend_result if result is None else result
    finally:
        if app.backend is not None:
            app.backend.join()
        global _ACTIVE
        if _ACTIVE is app:
            _ACTIVE = None
