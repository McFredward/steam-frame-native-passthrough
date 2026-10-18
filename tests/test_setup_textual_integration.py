"""Local UI entry-point and private runtime boundaries; no device operations."""
from contextlib import contextmanager, nullcontext
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'setup' / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


engine = load('textual_integration_engine', 'steam_link_setup.py')
bridge = load('textual_integration_bridge', 'stream_bridge_install.py')
vendor = load('textual_integration_vendor', 'steam_link_setup_vendor.py')
presentation = load('textual_integration_presentation', 'steam_link_setup_ui.py')
session_log = load('textual_integration_log', 'steam_link_setup_log.py')


class EntryPoint(unittest.TestCase):
    def test_actual_terminal_uses_one_textual_app_before_constructing_local_state(self):
        seen = []
        app = types.SimpleNamespace(run_setup=lambda callback, **kwargs: seen.append((callback, kwargs)) or 17)
        with mock.patch.object(engine.sys.stdin, 'isatty', return_value=True), \
                mock.patch.object(engine.sys.stdout, 'isatty', return_value=True), \
                mock.patch.object(engine, 'load_textual', return_value=app), \
                mock.patch.object(engine, 'Local', side_effect=AssertionError('not on UI thread')):
            self.assertEqual(engine.main(['--lang', 'de']), 17)
        self.assertEqual(len(seen), 1)
        self.assertTrue(callable(seen[0][0]))
        self.assertEqual(seen[0][1], {'lang': 'de'})

    def test_missing_framework_refuses_before_any_local_or_remote_operation(self):
        with mock.patch.object(engine.sys.stdin, 'isatty', return_value=True), \
                mock.patch.object(engine.sys.stdout, 'isatty', return_value=True), \
                mock.patch.object(engine, 'load_textual', side_effect=ImportError('absent')), \
                mock.patch.object(engine, 'Local', side_effect=AssertionError('no operation')), \
                mock.patch.object(engine.sys, 'stderr', io.StringIO()) as error:
            self.assertEqual(engine.main([]), 2)
        self.assertIn('No installation was started', error.getvalue())

    def test_presenter_imports_share_only_the_active_application(self):
        shared = object()
        with mock.patch.dict(sys.modules, {'_sfnp_setup_textual': types.SimpleNamespace(active_presenter=lambda **kw: shared)}):
            self.assertIs(presentation.Presenter(lang='de'), shared)
        with mock.patch.dict(sys.modules, {'_sfnp_setup_textual': types.SimpleNamespace(active_presenter=lambda **kw: None)}):
            plain = presentation.Presenter(io.StringIO(), input_enabled=False)
            self.assertIsInstance(plain, presentation.Presenter)


class ConsoleOwnership(unittest.TestCase):
    def test_only_cold_interactive_ssh_borrows_the_terminal(self):
        peer = object.__new__(bridge.Remote)
        peer.interactive = True
        peer.session = None
        self.assertTrue(peer.needs_terminal)
        peer.session = types.SimpleNamespace(closed=False)
        self.assertFalse(peer.needs_terminal)
        peer.session.closed = True
        self.assertTrue(peer.needs_terminal)
        peer.interactive = False
        self.assertFalse(peer.needs_terminal)

    def test_cached_rpcs_keep_the_application_running(self):
        events = []
        @contextmanager
        def suspend():
            events.append('suspend')
            yield
            events.append('resume')
        ui = types.SimpleNamespace(suspend=suspend)
        e = types.SimpleNamespace(nullcontext=nullcontext)
        for needs in (False, True, False):
            with bridge.terminal_scope(e, ui, types.SimpleNamespace(needs_terminal=needs)):
                events.append('rpc')
        self.assertEqual(events, ['rpc', 'suspend', 'rpc', 'resume', 'rpc'])

    def test_choice_and_confirmation_are_recorded_without_arbitrary_input(self):
        with tempfile.TemporaryDirectory(prefix='sfnp-textual-action-log-') as temporary:
            log = session_log.SessionLog.open(Path(temporary) / 'private', system='Linux')
            self.assertTrue(log.event('preflight', 'result', action='update', status='pending'))
            self.assertTrue(log.event('preflight', 'result', action='update', status='cancelled'))
            self.assertFalse(log.event('preflight', 'result', action='untrusted-password-input', status='ok'))
            path = log.path
            log.close()
            raw = path.read_text()
        self.assertNotIn('untrusted-password-input', raw)
        actions = [event for event in map(json.loads, raw.splitlines()) if 'action' in event]
        self.assertEqual([(e['action'], e['status']) for e in actions], [('update', 'pending'), ('update', 'cancelled')])


class Vendor(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sfnp-textual-loader-')
        self.addCleanup(self.temporary.cleanup)
        self.setup = Path(self.temporary.name) / 'setup'
        self.setup.mkdir()
        self.addCleanup(setattr, sys, 'path', sys.path[:])

    def make_vendor(self):
        target = self.setup / 'runtime/vendor'
        target.mkdir(parents=True)
        for name in ('textual', 'rich'):
            (target / name).mkdir()
            (target / name / '__init__.py').write_text('# owned test package\n')
        (target / 'SFNP-TEXTUAL-PROVENANCE.json').write_text(json.dumps({
            'schema': 'sfnp-textual-vendor-v1', 'packages': {'textual': {'version': '8.2.8'}}}))
        return target

    def test_complete_private_runtime_precedes_global_modules(self):
        target = self.make_vendor()
        self.assertEqual(vendor.enable_vendor(self.setup), target)
        self.assertEqual(sys.path[0], str(target))
        vendor.enable_vendor(self.setup)
        self.assertEqual(sys.path.count(str(target)), 1)

    def test_source_environment_allowed_but_partial_portable_bundle_refused(self):
        self.assertIsNone(vendor.enable_vendor(self.setup))
        (self.setup / 'runtime/python').mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            vendor.enable_vendor(self.setup)

    def test_malformed_or_wrong_runtime_pin_never_falls_back(self):
        target = self.make_vendor()
        for packages in ({'textual': {'version': '0.1'}}, {'textual': None}, {}):
            (target / 'SFNP-TEXTUAL-PROVENANCE.json').write_text(json.dumps({
                'schema': 'sfnp-textual-vendor-v1', 'packages': packages}))
            with self.assertRaises(ValueError):
                vendor.enable_vendor(self.setup)
        self.assertNotIn(str(target), sys.path)


if __name__ == '__main__':
    unittest.main()
