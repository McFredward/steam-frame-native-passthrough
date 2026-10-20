"""Arrow-key terminal tests; real POSIX PTYs and isolated Windows API contracts."""
import importlib.util
import io
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import time
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_tui', ROOT / 'setup/steam_link_setup_ui.py')
ui = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ui
spec.loader.exec_module(ui)


class Keys(unittest.TestCase):
    def test_windows_crt_extended_keys_enter_escape_and_ignored_input(self):
        data = iter(['\xe0', 'H', '\x00', 'P', '\xe0', 'G', '\xe0', 'O', '\r', '\n', '\x1b', 'x', '\xe0', 'X'])
        reader = ui._WindowsKeyReader(lambda: next(data))
        self.assertEqual([reader.read() for _ in range(9)],
                         ['up', 'down', 'home', 'end', 'enter', 'enter', 'escape', 'ignore', 'ignore'])
        for key, error in [('\x03', KeyboardInterrupt), ('\x1a', EOFError), ('', EOFError)]:
            with self.assertRaises(error):
                ui._WindowsKeyReader(lambda: key).read()

    @unittest.skipIf(os.name == 'nt', 'POSIX byte-reader contract')
    def test_posix_bounded_escape_sequences(self):
        cases = [(b'\x1b[A', 'up'), (b'\x1bOB', 'down'), (b'\x1b[1~', 'home'),
                 (b'\x1b[7~', 'home'), (b'\x1bOF', 'end'), (b'\x1b[8~', 'end'),
                 (b'\r', 'enter'), (b'\n', 'enter'), (b'x', 'ignore'),
                 (b'\x1b[Z', 'ignore'), (b'\x1b', 'escape')]
        for sequence, expected in cases:
            with self.subTest(sequence=sequence):
                descriptor, writer = os.pipe()
                try:
                    os.write(writer, sequence)
                    # Keep writer open: isolated Escape needs the short CSI
                    # disambiguation wait, not EOF interpreted as a CSI byte.
                    self.assertEqual(ui._PosixKeyReader(descriptor).read(), expected)
                finally:
                    os.close(descriptor)
                    os.close(writer)

    def test_windows_output_console_mode_is_restored_even_on_error(self):
        class Function:
            def __init__(self, callback):
                self.callback = callback
            def __call__(self, *args):
                return self.callback(*args)
        calls = []
        def query(handle, value):
            value._obj.value = 3
            return 1
        kernel = types.SimpleNamespace(GetConsoleMode=Function(query),
                                       SetConsoleMode=Function(lambda h, mode: calls.append((h, mode)) or 1))
        crt = types.SimpleNamespace(get_osfhandle=lambda fd: 99, getwch=lambda: '\r')
        stream = io.StringIO()
        stream.fileno = lambda: 41
        with mock.patch.dict(sys.modules, {'msvcrt': crt}), \
                mock.patch.object(ui.os, 'name', 'nt'), \
                mock.patch.object(ui.ctypes, 'WinDLL', return_value=kernel, create=True):
            terminal = ui._ArrowTerminal(None, stream)
            with self.assertRaisesRegex(RuntimeError, 'owned test failure'):
                with terminal:
                    self.assertEqual(terminal.reader.read(), 'enter')
                    raise RuntimeError('owned test failure')
        self.assertTrue(terminal.restored)
        self.assertEqual(calls, [(99, 7), (99, 3)])

    def test_windows_reader_initialization_failure_restores_already_set_mode(self):
        class Function:
            def __init__(self, callback):
                self.callback = callback
            def __call__(self, *args):
                return self.callback(*args)
        calls = []
        def query(_handle, value):
            value._obj.value = 3
            return 1
        kernel = types.SimpleNamespace(GetConsoleMode=Function(query),
                                       SetConsoleMode=Function(lambda h, mode: calls.append(mode) or 1))
        stream = io.StringIO()
        stream.fileno = lambda: 41
        with mock.patch.dict(sys.modules, {'msvcrt': types.SimpleNamespace(get_osfhandle=lambda fd: 99)}), \
                mock.patch.object(ui.os, 'name', 'nt'), \
                mock.patch.object(ui.ctypes, 'WinDLL', return_value=kernel, create=True), \
                mock.patch.object(ui, '_WindowsKeyReader', side_effect=OSError('reader unavailable')):
            with self.assertRaises(OSError):
                ui._ArrowTerminal(None, stream).__enter__()
        self.assertEqual(calls, [7, 3])

    def test_windows_console_mode_refusal_never_changes_input_or_outputs_ansi(self):
        class Function:
            def __init__(self, result):
                self.result = result
            def __call__(self, *_):
                return self.result
        kernel = types.SimpleNamespace(GetConsoleMode=Function(0), SetConsoleMode=Function(0))
        crt = types.SimpleNamespace(get_osfhandle=lambda fd: 99)
        stream = io.StringIO()
        stream.fileno = lambda: 41
        with mock.patch.dict(sys.modules, {'msvcrt': crt}), \
                mock.patch.object(ui.os, 'name', 'nt'), \
                mock.patch.object(ui.ctypes, 'WinDLL', return_value=kernel, create=True):
            with self.assertRaises(OSError):
                ui._ArrowTerminal(None, stream).__enter__()
        self.assertEqual(stream.getvalue(), '')


class Menus(unittest.TestCase):
    def make(self, responses=()):
        values = iter(responses)
        stream = io.StringIO()
        result = ui.Presenter(stream, lambda _: next(values), lang='en', input_enabled=True, width=22)
        return result, stream

    def test_bundled_link_label_refers_to_known_steamvr_without_inventing_own_version(self):
        for lang in ('en','de'):
            text=lambda key:ui.TEXT[lang][key]
            record={'compatible':True,'runtime_versions':{'steamvr':{'version':None,'build':'1789498647'},'steam_link':{'version':None,'build':None}}}
            rows=ui.runtime_labels(text,record)
            self.assertEqual(rows[0],'SteamVR: '+text('runtime_build').format(build='1789498647'))
            self.assertEqual(rows[1],'Steam Link: '+text('runtime_bundled'))
            self.assertNotIn('1789498647',rows[1])
            self.assertIsNone(record['runtime_versions']['steam_link']['version'])
            self.assertEqual(ui.runtime_labels(text,{**record,'compatible':False})[1],'Steam Link: '+text('runtime_bundled'))
            record['runtime_versions']['steam_link']['version']='9.8.7'
            self.assertEqual(ui.runtime_labels(text,record)[1],'Steam Link: 9.8.7')

    def test_runtime_labels_are_distinct_from_patch_versions_and_sanitize_unknown_metadata(self):
        for lang in ('en','de'):
            stream=io.StringIO();presenter=ui.Presenter(stream,lang=lang,width=80)
            record={'installed':True,'installed_version':'0.2.1','target_version':'0.2.2',
                    'runtime_versions':{'steamvr':{'version':'2.18.2','build':'1791331621'},
                                        'steam_link':{'version':None,'build':None}}}
            presenter.inventory(pc=record)
            output=stream.getvalue()
            self.assertIn('SteamVR: 2.18.2 (1791331621)',output)
            self.assertIn('Steam Link: '+presenter.text('runtime_bundled'),output)
            self.assertIn('0.2.1',output);self.assertIn('0.2.2',output)
            bad={'runtime_versions':{'steamvr':{'version':'\x1b[31m2.18.2','build':'oops'},
                                     'steam_link':{'version':'2.18.2\n[bad]','build':True}}}
            rows=ui.runtime_labels(presenter.text,bad)
            self.assertEqual(rows,['SteamVR: '+presenter.text('runtime_unknown'),'Steam Link: '+presenter.text('runtime_unknown')])
            self.assertEqual(ui.runtime_labels(presenter.text,{}),[])
            stream.seek(0);stream.truncate(0)
            presenter.inventory(pc={'runtime_versions':record['runtime_versions']})
            self.assertIn('SteamVR: 2.18.2 (1791331621)',stream.getvalue())
            self.assertNotIn(presenter.text('version_installed').split(':')[0],stream.getvalue())

    def test_patch_versions_are_shown_independently_in_console_status_and_plan(self):
        for lang in ('en','de'):
            presenter,stream=self.make();presenter.lang=lang
            pc={'installed':False,'installed_version':None,'target_version':'0.2.0'}
            frame={'installed':True,'installed_version':None,'target_version':'0.2.1'}
            presenter.inventory(pc=pc,frame=frame);presenter.version_plan(pc=pc,frame=frame)
            output=' '.join(stream.getvalue().split())
            self.assertIn(presenter.text('version_missing'),output)
            self.assertIn(presenter.text('version_unknown'),output)
            self.assertIn('0.2.0',output);self.assertIn('0.2.1',output)
            self.assertIn('→',output)

    def test_generalized_actions_plain_pipe_can_select_tenth_item(self):
        actions = ['install', 'update', 'repair', 'uninstall', 'check', 'exit', 'retry', 'back', 'rollback', 'continue']
        presenter, stream = self.make(['10'])
        self.assertEqual(presenter.choose_action(actions, recommended='check'), 'continue')
        self.assertNotIn('\x1b', stream.getvalue())
        self.assertTrue(all(len(line) < 22 for line in stream.getvalue().splitlines()))
        for action in ('retry', 'back', 'rollback', 'continue'):
            self.assertIn('action_' + action, ui.TEXT['en'])
            self.assertIn('action_' + action + '_description', ui.TEXT['de'])

    def test_confirmation_is_explicit_default_no_and_never_echoes_input(self):
        for answer, expected in [('', False), ('yes', True), ('JA', True), ('no', False), ('PASSWORD', False)]:
            presenter, stream = self.make([answer])
            self.assertEqual(presenter.confirm('bridge_confirm'), expected)
            self.assertNotIn('PASSWORD', stream.getvalue())
            self.assertNotIn('\x1b', stream.getvalue())
        presenter, stream = self.make()
        presenter.prompting = False
        self.assertFalse(presenter.confirm('bridge_confirm'))
        self.assertEqual(stream.getvalue(), '')

    def test_picture_question_keeps_no_separate_from_cancel_on_plain_input(self):
        for answer,expected in [('',False),('yes',True),('JA',True),('no',False),('nein',False),('cancel',None),('abbrechen',None)]:
            with self.subTest(answer=answer):
                presenter,stream=self.make([answer])
                self.assertIs(presenter.confirm('bridge_picture'),expected)
                self.assertIn('Choose No',stream.getvalue().replace('\n',' '))
                self.assertNotIn('\x1b',stream.getvalue())
        presenter,stream=self.make()
        presenter.prompting=False
        self.assertIsNone(presenter.confirm('bridge_picture'))
        self.assertEqual(stream.getvalue(),'')

    def test_terminal_needs_both_ttys_and_keyboard_permission(self):
        presenter, _ = self.make()
        presenter.interactive = True
        source = types.SimpleNamespace(isatty=lambda: False)
        with mock.patch.object(ui.sys, 'stdin', source), mock.patch.dict(os.environ, {'TERM': 'xterm'}):
            self.assertFalse(presenter._arrow_capable())
        source.isatty = lambda: True
        with mock.patch.object(ui.sys, 'stdin', source), mock.patch.dict(os.environ, {'TERM': 'xterm'}):
            self.assertTrue(presenter._arrow_capable())
            presenter.prompting = False
            self.assertFalse(presenter._arrow_capable())

    def test_arrow_panels_single_choice_narrow_width_and_cancel_without_default(self):
        presenter, _ = self.make()
        for width in (12, 22, 80):
            presenter._terminal_width = width
            choices = [('check', presenter.text('action_check'), presenter.text('action_check_description'))]
            lines = presenter._menu_lines(choices, 0, 'check', 'actions')
            self.assertTrue(all(len(line) < width for line in lines), lines)
        events = []
        class Terminal:
            restored = True
            def __init__(self, *_):
                self.reader = types.SimpleNamespace(read=lambda: 'enter')
            def __enter__(self):
                return self
            def __exit__(self, *_):
                pass
            def draw(self, p, lines):
                events.extend(lines)
        with mock.patch.object(ui, '_ArrowTerminal', Terminal):
            self.assertIsNone(presenter._arrow_choose([('install', 'Install', '')], None))
        self.assertIn('No action selected.', '\n'.join(events))

    def test_all_automation_copy_is_localized_without_inspection_or_legacy_regression(self):
        self.assertEqual(set(ui.TEXT['en']), set(ui.TEXT['de']))
        required = ('bridge_install_plan', 'bridge_remove_plan', 'bridge_partial', 'bridge_stopping',
                    'bridge_starting', 'bridge_rolled_back', 'bridge_retry_question', 'bridge_health_bad',
                    'bridge_health_ok', 'bridge_picture', 'bridge_reconnect', 'bridge_retry_ambiguous')
        for lang in ('en', 'de'):
            for key in required:
                self.assertTrue(ui.TEXT[lang][key])
        self.assertIn('tries to restore the previous setup and restart its interface', ui.TEXT['en']['bridge_install_plan'])
        self.assertIn('den vorherigen Zustand wiederherzustellen und dessen Oberfläche neu zu starten', ui.TEXT['de']['bridge_install_plan'])
        self.assertIn('no patch will be installed or removed', ui.TEXT['en']['inspection'])
        self.assertIn('not restarted', ui.TEXT['en']['remove_done'])
        self.assertIn('while your Frame interface stays running', ui.TEXT['en']['bridge_install_plan'])
        self.assertIn('bei laufender Frame-Oberfläche vorbereitet', ui.TEXT['de']['bridge_install_plan'])
        self.assertNotIn('Close SteamVR on the PC and Steam on the Frame', str(ui.TEXT))


@unittest.skipIf(os.name == 'nt', 'POSIX PTY terminal restoration; Windows CRT/mode contracts above')
class ActualPty(unittest.TestCase):
    def run_menu(self, expression, keys, *, failed_reader=False, spinner=False):
        import fcntl
        import pty
        import termios
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 88, 0, 0))
        before = termios.tcgetattr(slave)
        lines = ['import json,sys,termios', 'sys.path.insert(0,' + repr(str(ROOT / 'setup')) + ')',
                 'import steam_link_setup_ui as ui', 'p=ui.Presenter(lang="en")']
        if failed_reader:
            lines.append('ui._PosixKeyReader.read=lambda self: (_ for _ in ()).throw(OSError("owned failure"))')
        if spinner:
            lines.extend(['with p.progress("pc_check"):', ' value=' + expression])
        else:
            lines.append('value=' + expression)
        lines.extend(['print("RESULT="+json.dumps(value))', 'print("FINISHED",flush=True)'])
        child = subprocess.Popen([sys.executable, '-I', '-B', '-c', '\n'.join(lines)],
                                 stdin=slave, stdout=slave, stderr=slave,
                                 env={**os.environ, 'TERM': 'xterm'})
        data = bytearray()
        deadline = time.monotonic() + 5
        sent = failed_reader
        try:
            while child.poll() is None and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try:
                        data.extend(os.read(master, 16384))
                    except OSError:
                        break
                if not sent and b'Esc: cancel.' in data:
                    # Verify the spinner is paused while a real key menu waits.
                    while select.select([master], [], [], .03)[0]:
                        data.extend(os.read(master, 16384))
                    if spinner:
                        self.assertFalse(select.select([master], [], [], .2)[0])
                    os.write(master, keys)
                    sent = True
            if child.poll() is None:
                child.kill()
            code = child.wait(timeout=2)
            while select.select([master], [], [], .03)[0]:
                try:
                    data.extend(os.read(master, 16384))
                except OSError:
                    break
            self.assertEqual(code, 0, data.decode(errors='replace'))
            self.assertIn(b'FINISHED', data)
            self.assertEqual(termios.tcgetattr(slave), before, 'Terminal settings must be restored')
            result = bytes(data).split(b'RESULT=')[-1].splitlines()[0]
            return json.loads(result)
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()
            os.close(master)
            os.close(slave)

    def test_actual_arrow_home_end_enter_select_expected_offered_action(self):
        value = self.run_menu('p.choose_action(["check","uninstall","retry","back","rollback","continue","exit"],"check")',
                              b'\x1b[B\x1b[B\x1b[F\x1b[A\x1b[H\x1b[B\r')
        self.assertEqual(value, 'uninstall')

    def test_actual_escape_cancels_and_restores_terminal(self):
        self.assertIsNone(self.run_menu('p.choose_action(["check","install"],"check")', b'\x1b'))

    def test_actual_destructive_permission_requires_move_then_enter(self):
        self.assertFalse(self.run_menu('p.confirm_uninstall()', b'\r'))
        self.assertTrue(self.run_menu('p.confirm_uninstall()', b'\x1b[B\r'))
        self.assertFalse(self.run_menu('p.confirm("bridge_confirm")', b'\x1b'))

    def test_actual_picture_yes_no_and_cancel_have_distinct_results(self):
        self.assertIs(self.run_menu('p.confirm("bridge_picture")',b'\r'),False)
        self.assertIs(self.run_menu('p.confirm("bridge_picture")',b'\x1b[B\r'),True)
        self.assertIsNone(self.run_menu('p.confirm("bridge_picture")',b'\x1b[A\r'))
        self.assertIsNone(self.run_menu('p.confirm("bridge_picture")',b'\x1b'))

    def test_actual_reader_failure_restores_terminal_and_cancels(self):
        self.assertIsNone(self.run_menu('p.choose_action(["check","install"],"check")', b'', failed_reader=True))

    def test_actual_progress_is_suspended_until_key_selection_finishes(self):
        self.assertEqual(self.run_menu('p.choose_action(["retry","exit"],"retry")', b'\r', spinner=True), 'retry')


if __name__ == '__main__':
    unittest.main()
