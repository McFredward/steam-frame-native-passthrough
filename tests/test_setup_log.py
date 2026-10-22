"""Physical, bounded session-log checks; no network, Steam, secrets or device changes."""
import errno
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_setup_log', ROOT / 'setup/steam_link_setup_log.py')
log = importlib.util.module_from_spec(spec)
spec.loader.exec_module(log)


class SessionLogs(unittest.TestCase):
    def runtime_checks(self):
        return {'pc': {'vrlink': True, 'vrpathreg': True, '_runtime_versions': False},
                'frame': {'vrlink': True, 'compositor': True, 'fragment': False,
                          'openvr': True, '_runtime_versions': True}}

    def test_runtime_checks_retain_only_fixed_boolean_match_results(self):
        session = self.open()
        checks = self.runtime_checks()
        self.assertTrue(session.event('preflight', 'result', status='pending',
                                     runtime_checks=checks))
        recorded = self.entries(session)[-1]
        self.assertEqual(recorded['schema'], log.SCHEMA)
        self.assertEqual(recorded['runtime_checks'], checks)
        self.assertEqual(set(recorded), {'schema', 'sequence', 'utc', 'stage',
                                         'kind', 'status', 'runtime_checks'})
        checks['pc']['_runtime_versions'] = True
        self.assertFalse(self.entries(session)[-1]['runtime_checks']['pc']['_runtime_versions'])
        self.assertTrue(session.event('preflight', 'result', status='ok',
                                     runtime_checks=checks))
        self.assertTrue(self.entries(session)[-1]['runtime_checks']['pc']['_runtime_versions'])

    def test_runtime_checks_reject_unknown_roles_keys_and_raw_observations(self):
        session = self.open()
        before = session.path.read_bytes()
        invalid = [[], {}, {'pc': self.runtime_checks()['pc']},
                   {**self.runtime_checks(), 'password': 'SECRET'}]
        for role in ('pc', 'frame'):
            keys = tuple(self.runtime_checks()[role])
            for key in keys:
                for value in (0, 1, None, 'SECRET', {'matches': True, 'path': 'SECRET'}, [True]):
                    checks = self.runtime_checks()
                    checks[role][key] = value
                    invalid.append(checks)
            checks = self.runtime_checks()
            checks[role]['path'] = 'SECRET'
            invalid.append(checks)
            checks = self.runtime_checks()
            del checks[role][keys[0]]
            invalid.append(checks)
        for checks in invalid:
            with self.subTest(checks=checks):
                self.assertFalse(session.event('preflight', 'result', runtime_checks=checks))
                self.assertEqual(session.last_event_error, 'invalid_event')
                self.assertTrue(session.available)
        self.assertEqual(session.path.read_bytes(), before)
        self.assertNotIn(b'SECRET', session.path.read_bytes())
        self.assertTrue(session.event('preflight', 'result', status='ok'))
        self.assertNotIn('runtime_checks', self.entries(session)[-1])

    def test_component_versions_are_bounded_metadata_without_a_transcript(self):
        session=self.open()
        versions={'pc':{'installed':True,'installed_version':'0.2.0','target_version':'0.2.1'},
                  'frame':{'installed':False,'installed_version':None,'target_version':'0.2.1'}}
        self.assertTrue(session.event('preflight','result',status='ok',versions=versions))
        self.assertEqual(self.entries(session)[-1]['versions'],versions)
        before=session.path.read_bytes()
        for bad in ('password=secret','0.2.0\nsecret','0.2.0'+'a'*81):
            versions['pc']['installed_version']=bad
            self.assertFalse(session.event('preflight','result',versions=versions))
        self.assertEqual(session.path.read_bytes(),before)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sfnp-log-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / log.ROOT_NAME
        self.sessions = []

    def open(self, root=None):
        session = log.SessionLog.open(self.root if root is None else root)
        self.sessions.append(session)
        self.addCleanup(session.close)
        return session

    def entries(self, session):
        return [json.loads(line) for line in session.path.read_text(encoding='ascii').splitlines()]

    def test_important_filters_polling_but_preserves_failure_recovery_and_completion(self):
        session=log.SessionLog.open(self.root,level='Important');self.addCleanup(session.close)
        initial=session.path.read_bytes()
        for _ in range(200):self.assertTrue(session.event('frame_inventory','result',status='ok',command='bridge-inventory'))
        self.assertEqual(session.path.read_bytes(),initial)
        self.assertTrue(session.record_failure('pc_install',TimeoutError()))
        self.assertTrue(session.event('rollback','recovery',reason='recovery_retained'))
        self.assertTrue(session.event('complete','complete',status='ok'))
        entries=self.entries(session)
        self.assertEqual([e['kind'] for e in entries],['start','failure','recovery','complete'])
        self.assertEqual([e['sequence'] for e in entries],list(range(4)))
        self.assertFalse(session.event('frame_inventory','result',command='SECRET'))
        self.assertNotIn(b'SECRET',session.path.read_bytes())

    def test_real_unique_persistent_private_file(self):
        first = self.open()
        second = self.open()
        self.assertTrue(first.available, first.failure_code)
        self.assertTrue(second.available, second.failure_code)
        self.assertNotEqual(first.path, second.path)
        self.assertEqual(first.path.parent, self.root / 'logs')
        self.assertEqual(self.entries(first)[0]['stage'], 'session')
        first.close()
        self.assertTrue(first.path.is_file())
        self.assertFalse(first.event('complete', 'complete', status='ok'))
        self.assertEqual(first.last_event_error, 'closed')
        if os.name != 'nt':
            self.assertEqual(stat.S_IMODE(first.path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(first.path.parent.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(self.root.stat().st_mode), 0o700)

    def test_default_root_matches_engine_platform_environment(self):
        with mock.patch.dict(os.environ, {'LOCALAPPDATA': str(Path(self.temp.name) / 'win'),
                                         'XDG_DATA_HOME': str(Path(self.temp.name) / 'xdg')}):
            self.assertEqual(log.default_root('Windows'), Path(self.temp.name) / 'win' / log.ROOT_NAME)
            self.assertEqual(log.default_root('Linux'), Path(self.temp.name) / 'xdg' / log.ROOT_NAME)
        # Merely finding a root/importing does not create it.
        self.assertFalse((Path(self.temp.name) / 'win').exists())
        self.assertFalse((Path(self.temp.name) / 'xdg').exists())

    def test_all_fields_and_exact_known_recovery_paths_are_retained(self):
        session = self.open()
        paths = {'pc': str(self.root / 'bridge-install.json'),
                 'local': str(self.root / 'setup-last.log'),
                 'frame': '/home/steamos/.local/share/SteamLinkNativePassthrough/stream-bridge/install.json'}
        self.assertTrue(session.event('frame_prepare', 'recovery', status='partial',
                                     command='bridge-prepare', reason='check_0123456789abcdef',
                                     error_type='os_error', errno=13, winerror=32,
                                     recovery=paths))
        event = self.entries(session)[-1]
        self.assertEqual(event['recovery'], paths)
        self.assertEqual((event['reason'], event['errno'], event['winerror']),
                         ('check_0123456789abcdef', 13, 32))
        self.assertEqual(event['sequence'], 1)
        self.assertEqual(set(event), {'schema', 'sequence', 'utc', 'stage', 'kind', 'status',
                                     'command', 'reason', 'error_type', 'errno', 'winerror', 'recovery'})

    def test_windows_receipt_path_is_accepted_without_reading_remote_files(self):
        session = self.open()
        value = r'C:\Users\Player\AppData\Local\steam-link-native-passthrough\bridge-install.json'
        self.assertTrue(session.event('undo', 'recovery', recovery={'pc': value}))
        self.assertEqual(self.entries(session)[-1]['recovery']['pc'], value)
        self.assertTrue(session.event('undo', 'recovery', recovery={
            'frame': '/home/steamos/.config/SteamFrameNativePassthrough/install.json'}))

    def test_arbitrary_fields_and_potential_secrets_never_enter_file(self):
        session = self.open()
        before = session.path.read_bytes()
        invalid = [
            ('session', 'result', {'status': 'password=SECRET'}),
            ('SECRET', 'result', {}), ('session', 'SECRET', {}),
            ('session', 'result', {'command': 'ssh --password SECRET'}),
            ('session', 'result', {'reason': 'SECRET'}),
            ('session', 'result', {'reason': 'check_0123456789abcdef_SECRET'}),
            ('session', 'result', {'error_type': 'SECRET'}),
            ('session', 'result', {'errno': 'SECRET'}),
            ('session', 'result', {'errno': True}),
            ('session', 'result', {'winerror': 2**40}),
            ('session', 'recovery', {'recovery': {'password': 'SECRET'}}),
            ('session', 'recovery', {'recovery': {'pc': str(self.root / 'pairings/SECRET.json')}}),
            ('session', 'recovery', {'recovery': {'frame': '/home/SECRET.json'}}),
            ('session', 'recovery', {'recovery': {'frame': '/home/../SteamLinkNativePassthrough/stream-bridge/install.json'}}),
            ('session', 'recovery', {'recovery': {'pc': '//SECRET/steam-link-native-passthrough/bridge-install.json'}}),
            ('session', 'recovery', {'recovery': {'pc': str(self.root / 'bridge-install.json') + '\nSECRET'}}),
            ([], 'result', {}),
        ]
        for stage, kind, fields in invalid:
            with self.subTest(stage=stage, kind=kind, fields=fields):
                self.assertFalse(session.event(stage, kind, **fields))
                self.assertTrue(session.available)
                self.assertIn(session.last_event_error, ('invalid_event', 'invalid_path'))
        self.assertEqual(session.path.read_bytes(), before)
        self.assertNotIn(b'SECRET', session.path.read_bytes())

    def test_failures_use_only_categories_and_os_numbers(self):
        session = self.open()
        secret = 'PASSWORD_PAIRING_KEY_PRIVATE_STDERR'
        class PrivateError(ValueError):
            def __str__(self):
                raise AssertionError('Exception text must never be read')
        examples = [
            (PermissionError(errno.EACCES, secret, secret), 'permission_error', 'permission_denied'),
            (FileNotFoundError(errno.ENOENT, secret, secret), 'missing_error', 'missing'),
            (FileExistsError(errno.EEXIST, secret, secret), 'exists_error', 'already_exists'),
            (TimeoutError(secret), 'timeout_error', 'timeout'),
            (subprocess.TimeoutExpired(secret, 3, output=secret, stderr=secret), 'timeout_error', 'timeout'),
            (subprocess.CalledProcessError(13, secret, secret, secret), 'process_error', 'remote_unavailable'),
            (InterruptedError(secret), 'interrupted_error', 'interrupted'),
            (KeyboardInterrupt(secret), 'cancelled_error', 'cancelled'),
            (PrivateError(secret), 'validation_error', 'invalid_input'),
            (RuntimeError(secret), 'unknown_error', 'unknown_error'),
        ]
        for error, category, reason in examples:
            self.assertTrue(session.record_failure('pc_install', error))
            value = self.entries(session)[-1]
            self.assertEqual((value['error_type'], value['reason']), (category, reason))
        self.assertNotIn(secret, session.path.read_text())
        self.assertTrue(session.record_failure('frame_prepare', PrivateError(secret),
                                              reason='check_0123456789abcdef'))
        self.assertEqual(self.entries(session)[-1]['reason'], 'check_0123456789abcdef')

    def test_hostile_exception_numeric_attributes_do_not_replace_failure(self):
        class Hostile(OSError):
            def __getattribute__(self, name):
                if name in ('errno', 'winerror'):
                    raise RuntimeError('PRIVATE_EXCEPTION_CONTENT')
                return super().__getattribute__(name)
        session = self.open()
        self.assertTrue(session.record_failure('frame_prepare', Hostile()))
        value = self.entries(session)[-1]
        self.assertEqual(value['error_type'], 'os_error')
        self.assertNotIn('errno', value)
        self.assertNotIn('winerror', value)
        self.assertNotIn('PRIVATE_EXCEPTION_CONTENT', session.path.read_text())

    def test_invalid_or_physical_file_parent_failure_is_visible_not_raised(self):
        relative = self.open(Path('relative-data'))
        self.assertFalse(relative.available)
        self.assertEqual(relative.failure_code, 'invalid_path')
        blocker = Path(self.temp.name) / 'ordinary-file'
        blocker.write_text('preserve')
        session = self.open(blocker / log.ROOT_NAME)
        self.assertFalse(session.available)
        # Windows reports a missing child below an ordinary file as path-not-
        # found; POSIX reports ENOTDIR. Both preserve the blocking file.
        self.assertEqual(session.failure_code, 'missing' if os.name == 'nt' else 'io_error')
        self.assertEqual(session.path.parent, blocker / log.ROOT_NAME / 'logs')
        self.assertEqual(blocker.read_text(), 'preserve')
        self.assertFalse(session.record_failure('frame_prepare', ValueError('SECRET')))

    def test_open_refusal_and_later_write_failure_do_not_obscure_operation(self):
        with mock.patch.object(log.os, 'open', side_effect=PermissionError('SECRET')):
            session = self.open()
        self.assertFalse(session.available)
        self.assertEqual(session.failure_code, 'permission_denied')
        good = self.open()
        with mock.patch.object(log.os, 'write', side_effect=OSError(errno.ENOSPC, 'SECRET')):
            self.assertFalse(good.record_failure('frame_prepare', ValueError('ORIGINAL')))
        self.assertFalse(good.available)
        self.assertEqual(good.failure_code, 'io_error')
        self.assertNotIn('SECRET', good.path.read_text())
        self.assertIsNone(good._fd)

    def test_file_name_collision_is_not_overwritten_or_retried(self):
        fixed = log.datetime.datetime(2026, 10, 18, tzinfo=log.datetime.timezone.utc)
        with mock.patch.object(log.secrets, 'token_hex', return_value='0' * 16), \
                mock.patch.object(log.datetime, 'datetime') as clock:
            clock.now.return_value = fixed
            first = self.open()
            original = first.path.read_bytes()
            second = self.open()
        self.assertTrue(first.available)
        self.assertFalse(second.available)
        self.assertEqual(second.failure_code, 'already_exists')
        self.assertEqual(first.path.read_bytes(), original)

    def symlink(self, source, target, directory=False):
        try:
            source.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest('Symlink creation unavailable: ' + type(error).__name__)

    def test_symlink_parent_cannot_redirect_new_log(self):
        real = Path(self.temp.name) / 'unrelated'
        real.mkdir()
        self.symlink(self.root, real, True)
        session = self.open()
        self.assertFalse(session.available)
        self.assertEqual(session.failure_code, 'redirected_path')
        self.assertEqual(list(real.iterdir()), [])

    def test_changed_file_link_or_replacement_stops_logging(self):
        session = self.open()
        original = session.path.read_bytes()
        replacement = session.path.with_suffix('.saved')
        if os.name == 'nt':
            # Windows denies replacing the live descriptor's path outright.
            # Check the real OS protection rather than assuming POSIX rename.
            with self.assertRaises(PermissionError):session.path.rename(replacement)
            self.assertEqual(session.path.read_bytes(),original)
            self.assertFalse(replacement.exists())
            self.assertTrue(session.event('undo','start',status='started'))
            return
        session.path.rename(replacement)
        session.path.write_bytes(b'foreign')
        self.assertFalse(session.event('undo', 'start', status='started'))
        self.assertEqual(session.failure_code, 'metadata_changed')
        self.assertEqual(session.path.read_bytes(), b'foreign')
        self.assertEqual(replacement.read_bytes(), original)
        other = self.open()
        target = Path(self.temp.name) / 'unrelated'
        target.write_text('preserve')
        other.path.unlink()
        self.symlink(other.path, target)
        self.assertFalse(other.event('undo', 'start'))
        self.assertEqual(other.failure_code, 'redirected_path')
        self.assertEqual(target.read_text(), 'preserve')

    @unittest.skipIf(os.name == 'nt', 'POSIX permission/hardlink guarantees')
    def test_world_readable_roots_permissions_and_hardlinks_are_refused(self):
        self.root.mkdir(mode=0o755)
        session = self.open()
        self.assertFalse(session.available)
        self.assertEqual(session.failure_code, 'permission_denied')
        self.root.chmod(0o700)
        good = self.open()
        original = good.path.read_bytes()
        os.link(good.path, good.path.with_suffix('.linked'))
        self.assertFalse(good.event('undo', 'result'))
        self.assertEqual(good.failure_code, 'metadata_changed')
        self.assertEqual(good.path.read_bytes(), original)
        other = self.open()
        other.path.chmod(0o644)
        self.assertFalse(other.event('undo', 'result'))
        self.assertEqual(other.failure_code, 'permission_denied')

    def test_bounded_log_has_one_limit_receipt_preserves_old_logs(self):
        session = self.open()
        before = session.path.read_bytes()
        with mock.patch.object(log, 'MAX_BYTES', 8192):
            for _ in range(1000):
                if not session.event('startup_health', 'stage', status='pending',
                                     runtime_checks=self.runtime_checks()):
                    break
            self.assertTrue(session.available)
            self.assertEqual(session.last_event_error, 'log_limit')
            self.assertLessEqual(session.path.stat().st_size, 8192)
            length = session.path.stat().st_size
            for _ in range(100):
                self.assertFalse(session.event('startup_health', 'stage', status='pending',
                                               runtime_checks=self.runtime_checks()))
            self.assertEqual(session.path.stat().st_size, length)
        events = self.entries(session)
        self.assertEqual(sum(event.get('reason') == 'log_limit' for event in events), 1)
        self.assertEqual(events[-1]['reason'], 'log_limit')
        self.assertTrue(session.path.read_bytes().startswith(before))
        second = self.open()
        self.assertEqual(session.path.stat().st_size, length)
        self.assertNotEqual(session.path, second.path)

    def test_fsync_failure_and_context_exit_are_safe(self):
        session = self.open()
        with mock.patch.object(log.os, 'fsync', side_effect=OSError(errno.ENOSPC, 'SECRET')):
            self.assertFalse(session.event('cleanup', 'result', status='failed'))
        self.assertEqual(session.failure_code, 'io_error')
        other = self.open()
        with self.assertRaisesRegex(RuntimeError, 'original failure'):
            with other:
                raise RuntimeError('original failure')
        self.assertIsNone(other._fd)

    def test_console_and_password_streams_are_never_captured(self):
        with mock.patch.object(sys, 'stdout') as stdout, mock.patch.object(sys, 'stderr') as stderr:
            session = self.open()
            self.assertTrue(session.event('pairing', 'stage', status='started'))
        stdout.write.assert_not_called()
        stderr.write.assert_not_called()


if __name__ == '__main__':
    unittest.main()
