"""Small terminal presenter and bounded, read-only Steam Frame discovery.

Names, neighbor entries and SSH ports are hints, never device identity. The
caller must validate a real Frame inventory before returning a non-None value.
No credentials, persistence, service changes or installation exist here.
"""
from __future__ import annotations

from contextlib import contextmanager, nullcontext
from dataclasses import dataclass
import concurrent.futures
import ctypes
import ipaddress
import io
import json
import locale
import ntpath
import os
from pathlib import Path
import platform
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading
import textwrap
import time

PRIVATE = tuple(ipaddress.ip_network(v) for v in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))
NAMES = ('frame.local', 'steamframe.local', 'steam-frame.local')
MAX_VALIDATIONS = 8
MAX_SCAN_HOSTS = 254
MAX_NEIGHBORS = 64
VIRTUAL = re.compile(r'(?i)(vpn|wireguard|tailscale|zerotier|virtual|vmware|vbox|hyper-v|docker|podman|loopback|^lo$|^wsl|^tun\d*|^tap\d*|^veth|^virbr|^br-)')
TEXT = {
    'en': {
        'bridge_stopping': 'Closing SteamVR on this PC; your Frame interface stays running',
        'bridge_starting': 'Briefly restarting Steam on the Frame and checking its startup',
        'bridge_pc_stopped': 'SteamVR on this PC stays closed. Start your game normally when you are ready.',
        'bridge_rolled_back': 'The previous setup has been restored and its startup checks passed. The log and recovery paths are kept below.',
        'bridge_retry_question': 'Would you like to try again?',
        'bridge_reconnect': 'Reconnecting to your Frame after the restart',
        'bridge_retry_ambiguous': 'The last operation could not be confirmed. Setup will check what changed before offering a safe retry.',
        'bridge_resuming_update': 'The previous setup is restored. Continuing with the update you already confirmed.',
        'bridge_inventory_refresh_failed': 'The operation finished, but the final status could not be refreshed. Run Check again to confirm the current installation.',
        'action_retry': 'Try again',
        'action_retry_description': 'Check the last result and retry the operation.',
        'action_back': 'Back',
        'action_back_description': 'Return to the previous step.',
        'action_rollback': 'Restore previous setup',
        'action_rollback_description': 'Undo the recorded changes and restore the previous setup.',
        'action_continue': 'Continue',
        'action_continue_description': 'Continue with the plan shown above.',
        'choice_arrow_help': 'Up/Down or Home/End: move. Enter: select. Esc: cancel.',
        'choice_confirm': 'Confirm your choice',
        'confirm_no': 'No - go back',
        'confirm_yes': 'Yes - continue',
        'confirm_cancel': 'Cancel - keep installed components',
        'confirm_remove': 'Remove installed components',
        'confirm_yesno_prompt': 'Continue? [y/N]: ',
        'bridge_current': 'The checked connection is already up to date. Nothing was changed or restarted.',
        'bridge_available': 'The new passthrough connection is available for this package.',
        'bridge_remove_plan': 'Setup prepares removal while your Frame interface stays running. It restores the previous settings, then briefly restarts Steam on the Frame and checks the startup. SteamVR on this PC may close; your game settings are kept.',
        'bridge_removed': 'The connection was removed and the previous settings restored.',
        'bridge_install_plan': 'Setup prepares files while your Frame interface stays running. SteamVR on this PC may close. When everything is ready, setup briefly restarts Steam on the Frame and checks its startup. Your game settings are kept. A failed startup automatically restores the previous setup and restarts it.',
        'bridge_confirm': 'Install this connection now?',
        'bridge_partial': 'Setup could not finish. You can retry or restore the previous setup. The log and recovery paths below are kept for you.',
        'setup_log': 'Setup log: ',
        'setup_log_unavailable': 'Setup log could not be created: ',
        'recovery_pc': 'PC recovery record (if created): ',
        'recovery_frame': 'Frame recovery record (if created): ',
        'bridge_installed': 'Installation complete. Steam and SteamVR on the Frame are running and their startup checks passed.',
        'bridge_recovery_incomplete': 'Recovery is not complete yet. Retry will continue restoring the original setup and running Frame interface. The recovery records remain available.',
        'bridge_recover_confirm': 'Restore the recorded original setup and restart its Frame interface now?',
        'bridge_started': 'Has the Frame interface returned and is it responsive?',
        'bridge_health_ok': 'The new startup passed the software checks. Please also check the picture in the headset.',
        'bridge_health_waiting': 'Startup is still being checked; no crash was confirmed. Retry checks again without restarting the Frame. You can also restore the previous setup.',
        'bridge_health_bad': 'The new startup failed its checks. Setup is restoring the previous setup and checking its restart.',
        'bridge_picture': 'Is the Frame picture normal and its interface usable?',
        'bridge_stop_first': 'Setup prepares recovery while your Frame interface stays running, then briefly restarts Steam and checks its startup. Recovery records are kept until restoration succeeds.',
        'bridge_continue': 'Continue with the supervised restoration? ',
        'bridge_unavailable': 'The new connection is not available yet. You can check or remove the existing Frame patch.',
        'remove_plan': 'This removes only the existing Frame patch. The PC stays unchanged, and Steam is not restarted. If SteamVR still has the patch loaded, stop it manually and run this setup again to finish.',
        'remove_done': 'The existing Frame patch has been removed. Steam was not restarted.',
        'remove_pending': 'The Frame patch is disabled, but removal is not finished. Stop SteamVR manually, then run this setup again. Recovery files are kept until restoration succeeds.',
        'confirm_uninstall': 'Remove the installed components shown above?',
        'confirm_remove_prompt': 'Type REMOVE to confirm (Enter to cancel): ',
        'confirm_remove_word': 'REMOVE',
        'actions': 'What would you like to do?',
        'action_install': 'Install',
        'action_install_description': 'Install the components included in this package.',
        'action_update': 'Update',
        'action_update_description': 'Update the existing installation with this package.',
        'action_repair': 'Repair',
        'action_repair_description': 'Check and restore the existing installation.',
        'action_uninstall': 'Uninstall',
        'action_uninstall_description': 'Remove the installed components.',
        'action_check': 'Check only',
        'action_check_description': 'Check this PC and Frame without changing them.',
        'action_exit': 'Exit',
        'action_exit_description': 'Close without making changes.',
        'recommended': 'recommended',
        'choice': 'Your choice',
        'choice_help': 'Enter = {default}.  0 = Exit.',
        'choice_invalid': 'Please enter one of the numbers shown, or 0 to exit.',
        'choice_cancelled': 'No action selected.',
        'pc_check': 'Checking SteamVR on this PC',
        'pc_ready': 'PC files match the supported version.',
        'pc_partial': 'One or more PC files could not be confirmed; details are in the report.',
        'receiver_ready': 'Frame software matches the supported version.',
        'receiver_changed': 'Frame software differs from the supported version; details are in the report.',
        'check_finish': 'Check complete. The new passthrough connection has not been installed. Existing Steam Link stays unchanged.',
        'report': 'Report saved: ',
        'done_prompt': 'Press Enter to close. ',
        'title': 'Steam Link Native Passthrough',
        'inspection': 'Just a check: nothing will be installed, paired or restarted.',
        'setup': 'We will check your PC and Frame before offering any changes.',
        'search': 'Looking for your Steam Frame',
        'nearby': 'Checking this local network',
        'verify': 'Checking the device',
        'found': 'Steam Frame connected.',
        'login': 'A likely Frame is available. OpenSSH may ask for its fingerprint or password.',
        'missing': 'We could not identify your Frame automatically. Keep it awake, on the same network, with SSH enabled.',
        'manual': 'Last option - Frame IP from its Wi-Fi settings (Enter to check only this PC): ',
        'invalid': 'That is not a local private IPv4 address. Checking only this PC.',
        'unverified': 'That device could not be confirmed as a Steam Frame. Checking only this PC.',
        'pc_only': 'Continuing with this PC only.',
        'done': 'done',
        'failed': 'could not finish',
        'cancelled': 'Setup stopped. Any recovery details are shown below.',
    },
    'de': {
        'bridge_stopping': 'SteamVR auf diesem PC wird beendet; die Frame-Oberfläche läuft weiter',
        'bridge_starting': 'Steam auf der Frame wird kurz neu gestartet und der Start geprüft',
        'bridge_pc_stopped': 'SteamVR auf diesem PC bleibt geschlossen. Starte dein Spiel wie gewohnt, sobald du bereit bist.',
        'bridge_rolled_back': 'Der vorherige Zustand ist wiederhergestellt und hat die Startprüfungen bestanden. Das Log und die Wiederherstellungspfade bleiben unten erhalten.',
        'bridge_retry_question': 'Möchtest du es erneut versuchen?',
        'bridge_reconnect': 'Nach dem Neustart wieder mit der Frame verbinden',
        'bridge_retry_ambiguous': 'Die letzte Aktion konnte nicht bestätigt werden. Die Einrichtung prüft zunächst die Änderungen und bietet dann einen sicheren neuen Versuch an.',
        'bridge_resuming_update': 'Der Ausgangszustand ist wiederhergestellt. Die bereits bestätigte Aktualisierung wird jetzt fortgesetzt.',
        'bridge_inventory_refresh_failed': 'Die Aktion ist abgeschlossen, aber die abschließende Statusanzeige konnte nicht aktualisiert werden. Prüfe den Installationsstand mit „Nur prüfen“ erneut.',
        'action_retry': 'Erneut versuchen',
        'action_retry_description': 'Das letzte Ergebnis prüfen und die Aktion erneut versuchen.',
        'action_back': 'Zurück',
        'action_back_description': 'Zum vorherigen Schritt zurückkehren.',
        'action_rollback': 'Vorherigen Zustand wiederherstellen',
        'action_rollback_description': 'Die protokollierten Änderungen zurücknehmen und den vorherigen Zustand wiederherstellen.',
        'action_continue': 'Weiter',
        'action_continue_description': 'Mit dem oben angezeigten Vorhaben fortfahren.',
        'choice_arrow_help': 'Hoch/Runter oder Pos1/Ende: bewegen. Enter: auswählen. Esc: abbrechen.',
        'choice_confirm': 'Auswahl bestätigen',
        'confirm_no': 'Nein - zurück',
        'confirm_yes': 'Ja - fortfahren',
        'confirm_cancel': 'Abbrechen - installierte Komponenten behalten',
        'confirm_remove': 'Installierte Komponenten entfernen',
        'confirm_yesno_prompt': 'Fortfahren? [j/N]: ',
        'bridge_current': 'Die geprüfte Verbindung ist bereits aktuell. Es wurde nichts geändert oder neu gestartet.',
        'bridge_available': 'Die neue Passthrough-Verbindung ist mit diesem Paket verfügbar.',
        'bridge_remove_plan': 'Das Entfernen wird vorbereitet, während die Frame-Oberfläche weiterläuft. Die vorherigen Einstellungen werden wiederhergestellt, danach wird Steam auf der Frame kurz neu gestartet und geprüft. SteamVR auf diesem PC wird gegebenenfalls beendet; deine Spieleinstellungen bleiben erhalten.',
        'bridge_removed': 'Die Verbindung wurde entfernt und die vorherigen Einstellungen wiederhergestellt.',
        'bridge_install_plan': 'Die Dateien werden vorbereitet, während die Frame-Oberfläche weiterläuft. SteamVR auf diesem PC wird gegebenenfalls beendet. Sobald alles bereit ist, startet die Einrichtung Steam auf der Frame kurz neu und prüft den Start. Deine Spieleinstellungen bleiben erhalten. Bei einem fehlgeschlagenen Start wird der vorherige Zustand automatisch wiederhergestellt und neu gestartet.',
        'bridge_confirm': 'Diese Verbindung jetzt installieren?',
        'bridge_partial': 'Die Einrichtung konnte nicht abgeschlossen werden. Du kannst es erneut versuchen oder den vorherigen Zustand wiederherstellen. Das Log und die Wiederherstellungspfade unten bleiben erhalten.',
        'setup_log': 'Setup-Log: ',
        'setup_log_unavailable': 'Setup-Log konnte nicht erstellt werden: ',
        'recovery_pc': 'PC-Wiederherstellungsdatei (falls erstellt): ',
        'recovery_frame': 'Frame-Wiederherstellungsdatei (falls erstellt): ',
        'bridge_installed': 'Installation abgeschlossen. Steam und SteamVR auf der Frame laufen und die Startprüfungen sind bestanden.',
        'bridge_recovery_incomplete': 'Die Wiederherstellung ist noch nicht abgeschlossen. Erneut versuchen setzt das Wiederherstellen des Ausgangszustands und der laufenden Frame-Oberfläche fort. Die Wiederherstellungsdateien bleiben erhalten.',
        'bridge_recover_confirm': 'Den gespeicherten Ausgangszustand jetzt wiederherstellen und die Frame-Oberfläche neu starten?',
        'bridge_started': 'Ist die Frame-Oberfläche zurückgekehrt und reagiert sie?',
        'bridge_health_ok': 'Der neue Start hat die Softwareprüfungen bestanden. Bitte zusätzlich das Bild in der Brille prüfen.',
        'bridge_health_waiting': 'Der Start wird noch geprüft; ein Absturz wurde nicht bestätigt. Erneut versuchen prüft weiter, ohne die Frame neu zu starten. Alternativ kannst du den vorherigen Zustand wiederherstellen.',
        'bridge_health_bad': 'Der neue Start hat die Prüfung nicht bestanden. Die Einrichtung stellt den vorherigen Zustand wieder her und prüft den Neustart.',
        'bridge_picture': 'Ist das Bild auf der Frame normal und ihre Oberfläche bedienbar?',
        'bridge_stop_first': 'Die Wiederherstellung wird bei laufender Frame-Oberfläche vorbereitet. Danach wird Steam kurz neu gestartet und geprüft. Die Wiederherstellungsdaten bleiben bis zum erfolgreichen Abschluss erhalten.',
        'bridge_continue': 'Mit der überwachten Wiederherstellung fortfahren? ',
        'bridge_unavailable': 'Die neue Verbindung ist noch nicht verfügbar. Du kannst den vorhandenen Frame-Patch prüfen oder entfernen.',
        'remove_plan': 'Es wird nur der vorhandene Frame-Patch entfernt. Der PC bleibt unverändert und Steam wird nicht neu gestartet. Falls SteamVR den Patch noch geladen hat, bitte manuell beenden und diese Einrichtung erneut starten, um das Entfernen abzuschließen.',
        'remove_done': 'Der vorhandene Frame-Patch wurde entfernt. Steam wurde nicht neu gestartet.',
        'remove_pending': 'Der Frame-Patch ist deaktiviert, aber noch nicht vollständig entfernt. Bitte SteamVR manuell beenden und diese Einrichtung erneut starten. Die Wiederherstellungsdateien bleiben bis zum erfolgreichen Abschluss erhalten.',
        'confirm_uninstall': 'Die oben angezeigten installierten Komponenten entfernen?',
        'confirm_remove_prompt': 'Zum Bestätigen ENTFERNEN eingeben (Enter zum Abbrechen): ',
        'confirm_remove_word': 'ENTFERNEN',
        'actions': 'Was möchtest du tun?',
        'action_install': 'Installieren',
        'action_install_description': 'Die in diesem Paket enthaltenen Komponenten installieren.',
        'action_update': 'Aktualisieren',
        'action_update_description': 'Die vorhandene Installation mit diesem Paket aktualisieren.',
        'action_repair': 'Reparieren',
        'action_repair_description': 'Die vorhandene Installation prüfen und wiederherstellen.',
        'action_uninstall': 'Deinstallieren',
        'action_uninstall_description': 'Die installierten Komponenten entfernen.',
        'action_check': 'Nur prüfen',
        'action_check_description': 'PC und Frame prüfen, ohne etwas zu ändern.',
        'action_exit': 'Beenden',
        'action_exit_description': 'Ohne Änderungen schließen.',
        'recommended': 'empfohlen',
        'choice': 'Deine Auswahl',
        'choice_help': 'Enter = {default}.  0 = Beenden.',
        'choice_invalid': 'Bitte eine angezeigte Nummer oder 0 zum Beenden eingeben.',
        'choice_cancelled': 'Keine Aktion ausgewählt.',
        'pc_check': 'SteamVR auf diesem PC wird geprüft',
        'pc_ready': 'Die PC-Dateien entsprechen der unterstützten Version.',
        'pc_partial': 'Einige PC-Dateien konnten nicht bestätigt werden; Einzelheiten stehen im Bericht.',
        'receiver_ready': 'Die Frame-Software entspricht der unterstützten Version.',
        'receiver_changed': 'Die Frame-Software weicht von der unterstützten Version ab; Einzelheiten stehen im Bericht.',
        'check_finish': 'Prüfung abgeschlossen. Die neue Passthrough-Verbindung wurde nicht installiert. Bestehendes Steam Link bleibt unverändert.',
        'report': 'Bericht gespeichert: ',
        'done_prompt': 'Zum Schließen Enter drücken. ',
        'title': 'Steam Link Native Passthrough',
        'inspection': 'Nur prüfen: Es wird nichts installiert, gekoppelt oder neu gestartet.',
        'setup': 'Wir prüfen PC und Frame, bevor Änderungen angeboten werden.',
        'search': 'Steam Frame wird gesucht',
        'nearby': 'Lokales Netzwerk wird geprüft',
        'verify': 'Gerät wird geprüft',
        'found': 'Steam Frame verbunden.',
        'login': 'Eine mögliche Frame ist erreichbar. OpenSSH fragt gegebenenfalls nach Fingerabdruck oder Passwort.',
        'missing': 'Die Frame wurde nicht automatisch erkannt. Bitte eingeschaltet lassen, im selben Netzwerk verbinden und SSH aktivieren.',
        'manual': 'Letzte Möglichkeit - Frame-IP aus den WLAN-Einstellungen (Enter für nur diesen PC): ',
        'invalid': 'Das ist keine lokale private IPv4-Adresse. Es wird nur dieser PC geprüft.',
        'unverified': 'Das Gerät konnte nicht als Steam Frame bestätigt werden. Es wird nur dieser PC geprüft.',
        'pc_only': 'Es wird nur dieser PC geprüft.',
        'done': 'fertig',
        'failed': 'nicht abgeschlossen',
        'cancelled': 'Einrichtung beendet. Etwaige Wiederherstellungsdetails stehen unten.',
    },
}


def language(override=None):
    if override is not None:
        return 'de' if str(override).lower().split('-')[0].split('_')[0] == 'de' else 'en'
    try:
        value = locale.getlocale()[0] or ''
    except (ValueError, TypeError):
        value = ''
    if not value:
        value = os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or os.environ.get('LANG', '')
    return 'de' if value.lower().startswith(('de', 'german')) else 'en'


class _WindowsKeyReader:
    """CRT console keys; no line input, terminal subprocess or dependency."""
    def __init__(self, getwch=None):
        if getwch is None:
            import msvcrt
            getwch = msvcrt.getwch
        self._getwch = getwch

    def read(self):
        char = self._getwch()
        if char in ('\x00', '\xe0'):
            return {'H': 'up', 'P': 'down', 'G': 'home', 'O': 'end'}.get(self._getwch(), 'ignore')
        if char == '\x03':
            raise KeyboardInterrupt()
        if char in ('', '\x1a'):
            raise EOFError()
        return {'\r': 'enter', '\n': 'enter', '\x1b': 'escape'}.get(char, 'ignore')


class _PosixKeyReader:
    def __init__(self, descriptor):
        self.descriptor = descriptor

    def read(self):
        char = os.read(self.descriptor, 1)
        if not char or char == b'\x04':
            raise EOFError()
        if char == b'\x03':
            raise KeyboardInterrupt()
        if char in (b'\r', b'\n'):
            return 'enter'
        if char != b'\x1b':
            return 'ignore'
        # This is only Escape/CSI disambiguation, never an operation timer.
        sequence = bytearray()
        for _ in range(16):
            if not select.select([self.descriptor], [], [], .08)[0]:
                break
            part = os.read(self.descriptor, 1)
            if not part:
                break
            sequence.extend(part)
            if len(sequence) == 1 and part not in (b'[', b'O'):
                break
            if len(sequence) >= 2 and (part.isalpha() or part == b'~'):
                break
        return {b'[A': 'up', b'OA': 'up', b'[B': 'down', b'OB': 'down',
                b'[H': 'home', b'OH': 'home', b'[1~': 'home', b'[7~': 'home',
                b'[F': 'end', b'OF': 'end', b'[4~': 'end', b'[8~': 'end'}.get(bytes(sequence), 'escape' if not sequence else 'ignore')


class _ArrowTerminal:
    """Temporary console mode; always restore it before returning an action.

    ANSI cursor movement is restricted to an actual terminal. Windows explicitly
    enables/restores its own output console mode; pipes use the plain presenter.
    The menu never clears the screen, hides the cursor or enters another buffer.
    """
    def __init__(self, source, stream):
        self.source, self.stream = source, stream
        self._restore = None
        self.restored = False
        self.rows = 0

    def __enter__(self):
        if os.name == 'nt':
            import msvcrt
            from ctypes import wintypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            get_mode, set_mode = kernel.GetConsoleMode, kernel.SetConsoleMode
            get_mode.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
            get_mode.restype = wintypes.BOOL
            set_mode.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            set_mode.restype = wintypes.BOOL
            handle = msvcrt.get_osfhandle(self.stream.fileno())
            original = wintypes.DWORD()
            if not get_mode(handle, ctypes.byref(original)) or not set_mode(handle, original.value | 4):
                raise OSError('terminal mode unavailable')
            self._restore = lambda: bool(set_mode(handle, original.value))
            try:
                self.reader = _WindowsKeyReader()
            except BaseException:
                self.__exit__()
                raise
        else:
            import termios
            descriptor = self.source.fileno()
            original = termios.tcgetattr(descriptor)
            changed = list(original)
            changed[6] = list(original[6])
            changed[3] &= ~(termios.ICANON | termios.ECHO)
            changed[6][termios.VMIN], changed[6][termios.VTIME] = 1, 0
            termios.tcsetattr(descriptor, termios.TCSANOW, changed)
            self._restore = lambda: termios.tcsetattr(descriptor, termios.TCSANOW, original) is None
            self.reader = _PosixKeyReader(descriptor)
        return self

    def draw(self, presenter, lines):
        with presenter._lock:
            old = self.rows
            if old:
                presenter._write('\x1b[' + str(old) + 'A')
            for index in range(max(old, len(lines))):
                presenter._write('\r\x1b[2K' + (lines[index] if index < len(lines) else '') + '\n')
            if old > len(lines):
                presenter._write('\x1b[' + str(old - len(lines)) + 'A')
            self.rows = len(lines)

    def __exit__(self, *_):
        if self._restore is not None:
            try:
                self.restored = self._restore()
            except (OSError, ValueError):
                self.restored = False
        return False


class Presenter:
    """Arrow-key menus and animation exist only on a real terminal.

    Wrap any OpenSSH/password operation in suspend(). It clears the progress
    line before yielding; the background writer cannot race the prompt.
    """
    def __new__(cls, *args, **kwargs):
        # Only the actual entry point owns Textual's terminal lifetime. Discovery
        # and workflow retries reuse its one presenter across sibling imports.
        runtime = sys.modules.get('_sfnp_setup_textual')
        if runtime is not None:
            active = runtime.active_presenter(lang=kwargs.get('lang'))
            if active is not None:
                return active
        return super().__new__(cls)

    def __init__(self, stream=None, input_fn=input, lang=None, input_enabled=None, width=None):
        self.stream = sys.stdout if stream is None else stream
        self.input_fn = input_fn
        self.lang = language(lang)
        self.interactive = bool(getattr(self.stream, 'isatty', lambda: False)())
        self.prompting = bool(sys.stdin.isatty()) if input_enabled is None else bool(input_enabled)
        self._terminal_width = width
        self._lock = threading.RLock()
        self._tasks = []
        self._paused = 0
        self._width = 0
        self._stop = threading.Event()
        self._thread = None

    def text(self, key):
        return TEXT[self.lang][key]

    def _write(self, value):
        encoding = getattr(self.stream, 'encoding', None)
        if encoding:
            value = value.encode(encoding, errors='replace').decode(encoding)
        self.stream.write(value)
        self.stream.flush()

    def columns(self):
        """Leave the terminal's final column unused to avoid implicit wrapping."""
        width = self._terminal_width
        if width is None:
            try:
                width = os.get_terminal_size(self.stream.fileno()).columns
            except (AttributeError, OSError, ValueError, io.UnsupportedOperation):
                width = shutil.get_terminal_size(fallback=(80, 24)).columns
        # PTYs/redirected consoles may report an unset zero-sized window.
        # Treat that as unknown, rather than collapsing every paragraph.
        width = int(width)
        return max(12, min(width if width > 0 else 80, 120)) - 1

    def _paragraph(self, value, *, indent=2):
        prefix = ' ' * indent
        value = str(value)
        lines = textwrap.wrap(value, width=max(1, self.columns() - indent),
                              break_long_words=True, break_on_hyphens=False,
                              replace_whitespace=True) or ['']
        self._write(''.join(prefix + line + '\n' for line in lines))

    def _clear(self):
        if self._width:
            self._write('\r' + ' ' * min(self._width, self.columns()) + '\r')
            self._width = 0

    def say(self, key):
        with self._lock:
            self._clear()
            self._paragraph(self.text(key))
            self._write('\n')

    def section(self, key):
        with self._lock:
            self._clear()
            self._write('\n')
            self._paragraph(self.text(key))
            self._write('  ' + '-' * min(48, self.columns() - 2) + '\n\n')

    def status(self, key, state='info'):
        """A caller-classified status, without inferring installation readiness."""
        if state not in ('info', 'ok', 'warning'):
            raise ValueError('unknown status state')
        with self._lock:
            self._clear()
            marker = {'info': '[i]', 'ok': '[ok]', 'warning': '[!]'}[state]
            self._paragraph(marker + ' ' + self.text(key))
            self._write('\n')

    def path(self, value, key='report'):
        """Display the label and path separately; never append it to a status row."""
        with self._lock:
            self._clear()
            self._paragraph(self.text(key).strip())
            # Do not let a path containing control characters move the cursor.
            value = ''.join(c if c.isprintable() else '?' for c in str(value))
            self._paragraph(value, indent=4)
            self._write('\n')

    def banner(self, inspection=True):
        with self._lock:
            self._write('\n')
            if self.columns() >= 28:
                self._write('  +------------------------+\n'
                            '  |  [o]  STEAM FRAME  [o] |\n'
                            '  +------------------------+\n\n')
            else:
                self._paragraph('[o] FRAME [o]')
                self._write('\n')
            self._paragraph(self.text('title'))
            self._write('\n')
        self.say('inspection' if inspection else 'setup')

    def _animate(self, stopped):
        phase = 0
        while not stopped.wait(.12):
            with self._lock:
                if self._paused or not self._tasks:
                    continue
                value = '  ' + '|/-\\'[phase % 4] + ' ' + self._tasks[-1]
                width = self.columns()
                if len(value) > width:
                    value = value[:max(0, width - 3)] + '...'
                self._write('\r' + value + ' ' * max(0, min(self._width, width) - len(value)))
                self._width = len(value)
                phase += 1

    @contextmanager
    def progress(self, key):
        label = self.text(key)
        with self._lock:
            self._tasks.append(label)
            if self.interactive:
                if self._thread is None:
                    self._stop = threading.Event()
                    self._thread = threading.Thread(target=self._animate, args=(self._stop,), name='sfnp-progress', daemon=True)
                    self._thread.start()
            else:
                self._write('\n')
                self._paragraph(label + '...')
                self._write('\n')
        okay = False
        try:
            yield
            okay = True
        finally:
            with self._lock:
                self._tasks.pop()
                self._clear()
                self._paragraph('[' + self.text('done' if okay else 'failed') + '] ' + label)
                self._write('\n')
                worker = None
                if not self._tasks and self._thread is not None:
                    self._stop.set()
                    worker, self._thread = self._thread, None
            if worker is not None:
                worker.join()

    @contextmanager
    def suspend(self):
        with self._lock:
            self._paused += 1
            self._clear()
        try:
            yield
        finally:
            with self._lock:
                self._paused -= 1

    def _input(self, label):
        with self.suspend():
            with self._lock:
                if len(label) + 2 <= self.columns():
                    prompt = '  ' + label
                else:
                    self._paragraph(label)
                    prompt = '  > '
            return self.input_fn(prompt)

    def ask(self, key):
        if not self.prompting:
            return ''
        return self._input(self.text(key))

    def _arrow_capable(self):
        # Input permission and output animation are separate; a redirected
        # output with an interactive input still gets a usable plain prompt.
        return (self.prompting and self.interactive and
                bool(getattr(sys.stdin, 'isatty', lambda: False)()) and
                (os.name == 'nt' or os.environ.get('TERM', '') != 'dumb'))

    def _menu_lines(self, choices, selected, recommended, title):
        width = self.columns()
        try:
            height = os.get_terminal_size(self.stream.fileno()).lines or 24
        except (AttributeError, OSError, ValueError, io.UnsupportedOperation):
            height = 24
        visible = min(len(choices), max(2, min(height, 60) - 16))
        first = min(max(0, selected - visible + 1), max(0, len(choices) - visible))
        def short(value):
            value = ''.join(c if c.isprintable() else '?' for c in value)
            return value if len(value) <= width else value[:max(0, width - 3)] + '...'
        lines = ['  +' + '-' * max(1, width - 4) + '+']
        lines.extend('  ' + line for line in textwrap.wrap(self.text(title), width=max(1, width - 2))[:3])
        lines.append('')
        if first:
            lines.append('    ...')
        for index in range(first, first + visible):
            value = ('  > ' if index == selected else '    ') + choices[index][1]
            if choices[index][0] == recommended:
                value += ' [' + self.text('recommended') + ']'
            lines.append(short(value))
        if first + visible < len(choices):
            lines.append('    ...')
        description = choices[selected][2]
        if description:
            lines.append('')
            wrapped = textwrap.wrap(description, width=max(1, width - 4))
            lines.extend('    ' + line for line in wrapped[:2])
            if len(wrapped) > 2:
                lines[-1] = short(lines[-1][:-3] + '...')
        lines.append('')
        wrapped = textwrap.wrap(self.text('choice_arrow_help'), width=max(1, width - 2))
        lines.extend('  ' + line for line in wrapped[:3])
        lines.extend(['  +' + '-' * max(1, width - 4) + '+', ''])
        return lines

    def _arrow_choose(self, choices, default, recommended=None, title='actions'):
        """None means cancel; False means console mode could not be prepared."""
        if default is None:
            choices = [(None, self.text('choice_cancelled'), ''), *choices]
        terminal = _ArrowTerminal(sys.stdin, self.stream)
        try:
            terminal.__enter__()
        except (AttributeError, OSError, ValueError, io.UnsupportedOperation):
            return False
        selected = next((i for i, value in enumerate(choices) if value[0] == default), 0)
        answer = None
        try:
            while True:
                terminal.draw(self, self._menu_lines(choices, selected, recommended, title))
                key = terminal.reader.read()
                if key == 'escape':
                    break
                if key == 'enter':
                    answer = choices[selected][0]
                    break
                if key == 'up':
                    selected = max(0, selected - 1)
                elif key == 'down':
                    selected = min(len(choices) - 1, selected + 1)
                elif key == 'home':
                    selected = 0
                elif key == 'end':
                    selected = len(choices) - 1
        except (EOFError, KeyboardInterrupt, OSError):
            pass
        finally:
            terminal.__exit__()
        return answer if terminal.restored else None

    def choose_action(self, actions, recommended=None):
        """Offer only caller-approved actions; cancellation never implies a write.

        Real consoles use arrow keys and Enter. Pipes retain a numbered menu.
        This selects an action; the engine still owns its plan and permissions.
        """
        known = ('install', 'update', 'repair', 'uninstall', 'check', 'exit',
                 'retry', 'back', 'rollback', 'continue')
        if (type(actions) not in (list, tuple) or len(actions) > len(known) or
                any(type(action) is not str or action not in known for action in actions) or
                len(set(actions)) != len(actions)):
            raise ValueError('actions must be distinct supported action names')
        if not actions or not self.prompting:
            return None
        default = recommended if recommended in actions else ('check' if 'check' in actions else ('exit' if 'exit' in actions else None))
        with self.suspend():
            if self._arrow_capable():
                choices = [(action, self.text('action_' + action),
                            self.text('action_' + action + '_description')) for action in actions]
                selected = self._arrow_choose(choices, default, recommended)
                if selected is not False:
                    return selected
            self.section('actions')
            with self._lock:
                for number, action in enumerate(actions, 1):
                    label = self.text('action_' + action)
                    if action == recommended:
                        label += ' [' + self.text('recommended') + ']'
                    self._paragraph(str(number) + '. ' + label)
                    self._paragraph(self.text('action_' + action + '_description'), indent=5)
                    self._write('\n')
                self._paragraph(self.text('choice_help').format(default=self.text('action_' + (default or 'exit'))))
                self._write('\n')
            number = actions.index(default) + 1 if default is not None else 0
            label = self.text('choice') + ' [' + str(number) + ']: '
            for _ in range(3):
                try:
                    answer = self._input(label).strip()
                except (EOFError, KeyboardInterrupt):
                    self.say('choice_cancelled')
                    return None
                if not answer:
                    return default
                if answer.lower() in ('0', 'q', 'exit', 'cancel', 'abbrechen'):
                    return None
                if answer.isascii() and answer.isdigit() and 1 <= len(answer) <= 2 and 1 <= int(answer) <= len(actions):
                    return actions[int(answer) - 1]
                self.say('choice_invalid')
            self.say('choice_cancelled')
            return None

    def confirm(self, key):
        """Explicit permission for a displayed plan; No is always the default."""
        if not self.prompting:
            return False
        with self.suspend():
            self.say(key)
            if self._arrow_capable():
                choices = [('no', self.text('confirm_no'), ''),
                           ('yes', self.text('confirm_yes'), '')]
                result = self._arrow_choose(choices, 'no', title='choice_confirm')
                if result is not False:
                    return result == 'yes'
            try:
                answer = self._input(self.text('confirm_yesno_prompt')).strip().casefold()
            except (EOFError, KeyboardInterrupt):
                return False
            return answer in ('y', 'yes', 'j', 'ja')

    def confirm_uninstall(self):
        """A separate destructive confirmation, default Cancel on every console."""
        if not self.prompting:
            return False
        with self.suspend():
            self.say('confirm_uninstall')
            if self._arrow_capable():
                choices = [('cancel', self.text('confirm_cancel'), ''),
                           ('remove', self.text('confirm_remove'), '')]
                result = self._arrow_choose(choices, 'cancel', title='choice_confirm')
                if result is not False:
                    return result == 'remove'
            try:
                answer = self._input(self.text('confirm_remove_prompt')).strip()
            except (EOFError, KeyboardInterrupt):
                return False
            return answer.casefold() == self.text('confirm_remove_word').casefold()


@dataclass(frozen=True)
class Lan:
    address: str
    prefix: int
    interface: str
    gateway: str
    index: int = 0
    metric: int = 0

    @property
    def network(self):
        return ipaddress.ip_interface(self.address + '/' + str(self.prefix)).network


@dataclass(frozen=True)
class FoundFrame:
    host: str
    value: object
    origin: str


def private_ip(value):
    if type(value) is not str:
        return None
    try:
        address = ipaddress.ip_address(value)
    except (ValueError, TypeError):
        return None
    return address if address.version == 4 and any(address in network for network in PRIVATE) else None


def eligible_lan(lan):
    try:
        return (isinstance(lan, Lan) and type(lan.prefix) is int and 8 <= lan.prefix <= 30 and
                type(lan.interface) is str and re.fullmatch(r'[A-Za-z0-9 _.:()/-]{1,128}', lan.interface) is not None and
                not VIRTUAL.search(lan.interface) and private_ip(lan.address) is not None and
                private_ip(lan.gateway) is not None and ipaddress.ip_address(lan.gateway) in lan.network and
                any(lan.network.subnet_of(network) for network in PRIVATE))
    except (ValueError, TypeError):
        return False


def candidate_ip(value, lan=None):
    address = private_ip(value)
    if address is None:
        return None
    if lan is not None:
        if not eligible_lan(lan) or address not in lan.network or str(address) in (lan.address, lan.gateway):
            return None
        if address in (lan.network.network_address, lan.network.broadcast_address):
            return None
    return str(address)


def command_output(argv, *, timeout=3, limit=65536):
    """Bound/reap a fixed read-only OS command without background reader threads."""
    try:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, bufsize=0)
    except OSError:
        return b''
    result = bytearray()
    okay = True
    deadline = time.monotonic() + timeout
    try:
        if os.name == 'nt':
            import msvcrt
            from ctypes import wintypes
            peek = ctypes.WinDLL('kernel32', use_last_error=True).PeekNamedPipe
            peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                             wintypes.LPVOID, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
            peek.restype = wintypes.BOOL
            handle = msvcrt.get_osfhandle(child.stdout.fileno())
        while True:
            if time.monotonic() >= deadline:
                okay = False
                break
            if os.name == 'nt':
                available = wintypes.DWORD()
                if not peek(handle, None, 0, None, ctypes.byref(available), None):
                    break  # Broken pipe after process exit is normal EOF.
                if not available.value:
                    if child.poll() is not None:
                        break
                    time.sleep(.02)
                    continue
                count = min(4096, available.value)
            else:
                ready, _, _ = select.select([child.stdout], [], [], min(.05, max(0, deadline - time.monotonic())))
                if not ready:
                    continue
                count = 4096
            chunk = os.read(child.stdout.fileno(), count)
            if not chunk:
                break
            if len(result) + len(chunk) > limit:
                okay = False
                break
            result.extend(chunk)
    except (OSError, ValueError):
        okay = False
    finally:
        if not okay and child.poll() is None:
            child.kill()
        try:
            child.wait(timeout=max(.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
            okay = False
        child.stdout.close()
    return bytes(result) if okay and child.returncode == 0 else b''


def json_rows(raw, maximum=64):
    if not raw or len(raw) > 65536:
        return []
    try:
        value = json.loads(raw.decode('utf-8-sig'))
    except (ValueError, UnicodeError):
        return []
    rows = value if type(value) is list else [value]
    return rows if len(rows) <= maximum and all(type(row) is dict for row in rows) else []


def windows_directory():
    try:
        from ctypes import wintypes
        query = ctypes.WinDLL('kernel32', use_last_error=True).GetSystemDirectoryW
        query.argtypes = [wintypes.LPWSTR, wintypes.UINT]
        query.restype = wintypes.UINT
        buffer = ctypes.create_unicode_buffer(32768)
        size = query(buffer, len(buffer))
        return buffer.value if 0 < size < len(buffer) else None
    except (AttributeError, OSError):
        return None


WINDOWS_LAN = r"""[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false; @(Get-NetIPConfiguration | Where-Object { $_.NetAdapter.Status -eq 'Up' -and $_.NetAdapter.HardwareInterface -and -not $_.NetAdapter.Virtual -and $_.IPv4DefaultGateway } | Select-Object -First 8 | ForEach-Object { $c=$_; @($c.IPv4Address) | Select-Object -First 4 | ForEach-Object { [pscustomobject]@{address=$_.IPAddress;prefix=$_.PrefixLength;interface=$c.InterfaceAlias;index=$c.InterfaceIndex;gateway=$c.IPv4DefaultGateway.NextHop;metric=$c.NetIPv4Interface.InterfaceMetric;physical=$c.NetAdapter.HardwareInterface;virtual=$c.NetAdapter.Virtual} } }) | ConvertTo-Json -Compress"""
WINDOWS_NEIGHBORS = r"""[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false; @(Get-NetNeighbor -AddressFamily IPv4 | Where-Object { $_.State -in @('Reachable','Stale','Permanent') } | Select-Object -First 64 | ForEach-Object { [pscustomobject]@{address=$_.IPAddress;index=$_.InterfaceIndex} }) | ConvertTo-Json -Compress"""
WINDOWS_DNS = r"""[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false; @(Get-DnsClientCache | Where-Object { $_.Type -eq 1 -and $_.Entry -match '(^frame[.]local$|steam[-_]?frame)' } | Select-Object -First 32 | ForEach-Object { [pscustomobject]@{address=$_.Data;name=$_.Entry} }) | ConvertTo-Json -Compress"""


def _dns_name(data, offset):
    labels = []
    end = None
    seen = set()
    for _ in range(32):
        if offset >= len(data) or offset in seen:
            raise ValueError('DNS name bounds')
        seen.add(offset)
        length = data[offset]
        if length & 0xc0 == 0xc0:
            if offset + 1 >= len(data):
                raise ValueError('DNS pointer bounds')
            if end is None:
                end = offset + 2
            offset = ((length & 0x3f) << 8) | data[offset + 1]
            continue
        if length > 63 or offset + 1 + length > len(data):
            raise ValueError('DNS label bounds')
        offset += 1
        if not length:
            return '.'.join(labels).lower(), end if end is not None else offset
        labels.append(data[offset:offset + length].decode('ascii').lower())
        if sum(map(len, labels)) + len(labels) > 253:
            raise ValueError('DNS name bound')
        offset += length
    raise ValueError('DNS pointer cycle')


def mdns_addresses(data, names):
    """Read only requested A records. Name/port is still not Frame identity."""
    if len(data) < 12 or len(data) > 4096:
        return []
    _, flags, questions, answers, authorities, additional = struct.unpack_from('!6H', data)
    if not flags & 0x8000 or questions > 8 or answers + authorities + additional > 64:
        return []
    result = []
    try:
        offset = 12
        for _ in range(questions):
            _, offset = _dns_name(data, offset)
            offset += 4
            if offset > len(data):
                return []
        for _ in range(answers + authorities + additional):
            name, offset = _dns_name(data, offset)
            if offset + 10 > len(data):
                return []
            kind, klass, _, count = struct.unpack_from('!HHIH', data, offset)
            offset += 10
            if offset + count > len(data):
                return []
            if name in names and kind == 1 and klass & 0x7fff == 1 and count == 4:
                value = str(ipaddress.ip_address(data[offset:offset + count]))
                if private_ip(value) is not None:
                    result.append(value)
            offset += count
    except (ValueError, UnicodeError, struct.error):
        return []
    return list(dict.fromkeys(result))


def mdns(lan, names=NAMES):
    if not eligible_lan(lan):
        return []
    result = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.bind((lan.address, 0))
            client.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(lan.address))
            client.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            client.settimeout(.15)
            for name in names[:4]:
                question = b''.join(bytes([len(part)]) + part.encode('ascii') for part in name.split('.')) + b'\x00'
                client.sendto(struct.pack('!6H', 0, 0, 1, 0, 0, 0) + question + struct.pack('!HH', 1, 0x8001), ('224.0.0.251', 5353))
            end = time.monotonic() + .65
            for _ in range(32):
                if time.monotonic() >= end:
                    break
                try:
                    packet, _ = client.recvfrom(4096)
                except socket.timeout:
                    continue
                result.extend(mdns_addresses(packet, names))
    except OSError:
        return []
    return list(dict.fromkeys(value for value in result if candidate_ip(value, lan)))


class SystemSources:
    """Fixed OS commands; only a proven physical active private LAN may sweep."""
    def __init__(self, *, system=None, run=command_output, system_directory=None,
                 device_exists=None, multicast=mdns):
        self.system = platform.system() if system is None else system
        self.run = run
        self.directory = windows_directory() if self.system == 'Windows' and system_directory is None else system_directory
        self.device_exists = device_exists or (lambda name: (Path('/sys/class/net') / name / 'device').exists())
        self.multicast = multicast
        self.ip_tool = shutil.which('ip')
        self._resolved = {}

    def _powershell(self, command):
        if not self.directory:
            return []
        path = ntpath.join(self.directory, 'WindowsPowerShell', 'v1.0', 'powershell.exe')
        return json_rows(self.run([path, '-NoLogo', '-NoProfile', '-NonInteractive', '-Command', command]))

    def lan(self):
        choices = []
        if self.system == 'Windows':
            for row in self._powershell(WINDOWS_LAN):
                if row.get('physical') is not True or row.get('virtual') is not False:
                    continue
                try:
                    lan = Lan(row['address'], row['prefix'], row['interface'], row['gateway'], row['index'], row.get('metric') or 0)
                    if eligible_lan(lan) and type(lan.index) is int and type(lan.metric) is int:
                        choices.append(lan)
                except (KeyError, TypeError, ValueError):
                    continue
        elif self.system == 'Linux' and self.ip_tool:
            routes = json_rows(self.run([self.ip_tool, '-j', '-4', 'route', 'show', 'default']))
            for route in routes[:8]:
                name = route.get('dev', '')
                if type(name) is not str or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,32}', name) or VIRTUAL.search(name):
                    continue
                if not self.device_exists(name):
                    continue
                rows = json_rows(self.run([self.ip_tool, '-j', '-d', '-4', 'address', 'show', 'dev', name, 'up']))
                for row in rows:
                    if row.get('link_type') != 'ether' or type(row.get('linkinfo', {})) is not dict or row.get('linkinfo', {}).get('info_kind'):
                        continue
                    addresses = row.get('addr_info', [])
                    if type(addresses) is not list:
                        continue
                    for address in addresses[:8]:
                        try:
                            lan = Lan(address['local'], address['prefixlen'], name, route['gateway'], row['ifindex'], route.get('metric', 0))
                            if address.get('family') == 'inet' and eligible_lan(lan) and type(lan.index) is int and type(lan.metric) is int:
                                choices.append(lan)
                        except (KeyError, TypeError, ValueError):
                            continue
        return min(choices, key=lambda item: item.metric) if choices else None

    def resolve(self, name):
        # OS DNS/mDNS support is useful even when physical-LAN inventory is
        # unavailable. Run it in a bounded/reaped child: getaddrinfo has no
        # portable per-call deadline, and daemon threads would outlive a check.
        if type(name) is not str or len(name) > 253 or not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?', name):
            return []
        if name not in self._resolved:
            code = ('import socket,json,sys; '
                    'print(json.dumps(sorted({r[4][0] for r in '
                    'socket.getaddrinfo(sys.argv[1],22,socket.AF_INET,socket.SOCK_STREAM)})))')
            raw = self.run([sys.executable, '-I', '-c', code, name], timeout=1.25, limit=2048)
            try:
                values = json.loads(raw.decode('utf-8'))
            except (ValueError, UnicodeError):
                values = []
            self._resolved[name] = list(dict.fromkeys(value for value in values
                if type(value) is str and candidate_ip(value)))[:8] if type(values) is list else []
        return self._resolved[name][:]

    def named(self, lan):
        values = []
        for name in NAMES:
            values.extend(self.resolve(name))
        if eligible_lan(lan):
            values.extend(self.multicast(lan, NAMES))
        if self.system == 'Windows':
            for row in self._powershell(WINDOWS_DNS):
                if type(row.get('name')) is str and re.search(r'(?i)(^frame[.]local$|steam[-_]?frame)', row['name']):
                    values.append(row.get('address'))
        return list(dict.fromkeys(value for value in values if candidate_ip(value, lan)))[:8]

    def neighbors(self, lan):
        if not eligible_lan(lan):
            return []
        if self.system == 'Windows':
            rows = self._powershell(WINDOWS_NEIGHBORS)
            values = [row.get('address') for row in rows if row.get('index') == lan.index]
        elif self.system == 'Linux' and self.ip_tool:
            rows = json_rows(self.run([self.ip_tool, '-j', '-4', 'neigh', 'show', 'dev', lan.interface]), MAX_NEIGHBORS)
            values = [row.get('dst') for row in rows if type(row.get('state', [])) in (list, str) and not any(value in row.get('state', []) for value in ('FAILED', 'INCOMPLETE'))]
        else:
            values = []
        return list(dict.fromkeys(value for value in values if candidate_ip(value, lan)))[:MAX_NEIGHBORS]


def port_open(address, port=22, timeout=.15):
    try:
        with socket.create_connection((address, port), timeout=timeout):
            return True
    except OSError:
        return False


def scan(lan, connector=port_open):
    if not eligible_lan(lan):
        return []
    network = ipaddress.ip_network(lan.address + '/' + str(max(lan.prefix, 24)), strict=False)
    addresses = [str(address) for address in network.hosts() if candidate_ip(str(address), lan)][:MAX_SCAN_HOSTS]
    def check(address):
        try:
            return address if connector(address, 22, .15) else None
        except OSError:
            return None
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        return [value for value in pool.map(check, addresses) if value]


def find_frame(validate, *, cached_hosts=(), sources=None, presenter=None,
               allow_manual=True, connector=port_open):
    """Return only the caller's verified inventory, never an inferred Frame.

    validate(host, interactive=False) MUST use batch authentication/no prompts.
    It returns a caller-owned Frame value (e.g. remote+inventory), or None.
    At most eight batch validations and one automatic interactive validation
    occur. Unknown SSH hosts are never interactively prompted automatically.
    Cached/named hints may receive one OpenSSH prompt; an explicit manual
    address may receive one final prompt. The callback owns its own timeout.
    """
    ui = presenter or Presenter()
    sources = sources or SystemSources()
    seen = set()
    attempts = 0
    interactive_hint = None
    def validate_one(address, origin, interactive=False):
        try:
            with ui.suspend() if interactive else nullcontext():
                value = validate(address, interactive=interactive)
        except (OSError, ValueError):
            return None
        if value is not None and value is not False:
            ui.say('found')
            return FoundFrame(address, value, origin)
        return None
    def candidates(values, origin, strong=False):
        nonlocal attempts, interactive_hint
        for address in values:
            address = candidate_ip(address, lan)
            if not address or address in seen:
                continue
            seen.add(address)
            if attempts >= MAX_VALIDATIONS:
                break
            try:
                reachable = connector(address, 22, .15)
            except OSError:
                reachable = False
            if not reachable:
                continue
            attempts += 1
            found = validate_one(address, origin)
            if found:
                return found
            if strong and interactive_hint is None:
                interactive_hint = (address, origin)
        return None
    lan = None
    with ui.progress('search'):
        cached = []
        for hint in list(cached_hosts)[:4]:
            if candidate_ip(hint):
                cached.append(hint)
            else:
                cached.extend(sources.resolve(hint))
        found = candidates(cached, 'cached', True)
        if found:
            return found
        # Plain DNS names must work without a physical-interface table, e.g.
        # when Windows network inventory is slow or not available.
        found = candidates(sources.named(None), 'named', True)
        if found:
            return found
        lan = sources.lan()
        if not eligible_lan(lan):
            lan = None
        if lan is not None:
            found = candidates(sources.named(lan), 'named', True)
            if found:
                return found
        if interactive_hint and ui.prompting:
            ui.say('login')
            found = validate_one(*interactive_hint, interactive=True)
            if found:
                return found
    if lan is not None:
        with ui.progress('nearby'):
            found = candidates(sources.neighbors(lan), 'neighbor')
            if found:
                return found
            if attempts < MAX_VALIDATIONS:
                found = candidates(scan(lan, connector), 'local-scan')
                if found:
                    return found
    ui.say('missing')
    if not allow_manual or not ui.prompting:
        ui.say('pc_only')
        return None
    value = ui.ask('manual').strip()
    if not value:
        ui.say('pc_only')
        return None
    address = candidate_ip(value)
    if address is None:
        ui.say('invalid')
        return None
    found = validate_one(address, 'manual', interactive=True)
    if not found:
        ui.say('unverified')
    return found
