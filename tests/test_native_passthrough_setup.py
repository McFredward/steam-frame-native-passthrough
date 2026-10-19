#!/usr/bin/env python3
"""Retired transport maintenance and removal on owned temporary files; no device."""
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('owned_setup', ROOT / 'tools/native_passthrough_setup.py')
s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)


class System:
    def __init__(self):
        self.live = False
        self.reloads = 0
        self.native_calls = []
        self.settings = None
        self.persist = True

    def reload(self):
        self.reloads += 1
        return True

    def running(self):
        return self.live

    @contextlib.contextmanager
    def camera(self, package, metadata, runtime):
        self.native_calls.append('open-pinned')
        owner = self
        class Api:
            def write(self, value):
                owner.native_calls.append(('set', value))
                if owner.persist:
                    data = s.load(owner.settings)
                    data.setdefault('camera', {})[s.KEY] = value
                    s.put(owner.settings, s.encoded(data), s.read(owner.settings))
            def remove_override(self):
                owner.native_calls.append('remove-key')
                if owner.persist:
                    data = s.load(owner.settings)
                    data.setdefault('camera', {}).pop(s.KEY, None)
                    s.put(owner.settings, s.encoded(data), s.read(owner.settings))
        yield Api()


class SetupTests(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ, {'VR_CONFIG_PATH': '', 'VR_PATHREG_OVERRIDE': '', 'XDG_CONFIG_HOME': ''})
        environment.start()
        self.addCleanup(environment.stop)
        self.temp = tempfile.TemporaryDirectory(prefix='sfnp-setup-owned-')
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        self.home.mkdir(mode=0o700)
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir(mode=0o700)
        self.package = self.root / 'input'
        self.package.mkdir(mode=0o700)
        artifacts = {s.LIBRARY: b'OWNED-LIBRARY-NOT-A-NATIVE-PROOF', 'scene-presenter.spv': b'OWNED-SPV',
                     'tools/prepare_native_release.py': b'# owned fake verifier, never executed\n',
                     'tools/steamvr_camera_alpha.py': b'# owned fake camera, never executed\n',
                     'tools/pixel_snapshot_session.py': b'# owned fixture\n', 'pc/vrsettings_win.py': b'# fixture\n',
                     'compatibility.json': s.encoded({'runtime_files': {'openvr': {'sha256': '0' * 64}}})}
        self.metadata = {'schema': 'sfnp-native-package-v1', 'files': {n: {'bytes': len(b), 'sha256': s.digest(b)} for n, b in artifacts.items()}}
        for name, raw in {**artifacts, 'package.json': s.encoded(self.metadata)}.items():
            (self.package / name).parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            s.put(self.package / name, raw)
        self.unit = System()
        self.compatible = True
        self.checks = 0
        def verify(package, runtime):
            self.checks += 1
            if not self.compatible:
                raise s.SafetyError('unsupported runtime fingerprint: explicit owned fixture')
            manifest = s.load(package / 'package.json')
            for name, record in manifest['files'].items():
                raw = s.read(package / name)
                if s.digest(raw) != record['sha256']:
                    raise s.SafetyError('artifact hash mismatch')
            return manifest
        self.setup = s.Setup(self.home, system=self.unit, verifier=verify, runtime_factory=lambda: {'openvr': self.root / 'pinned'}, runtime_dir=self.runtime)
        self.settings = self.home / '.config/openvr/config/steamvr.vrsettings'
        os.close(s.directory(self.settings.parent, create=True))
        self.unit.settings = self.settings
        s.put(self.settings, s.encoded({'unrelated': {'keep': 42}, 'camera': {'other': 'keep'}}))
        self.secondary_settings = self.home / '.local/share/Steam/config/steamvr.vrsettings'
        os.close(s.directory(self.secondary_settings.parent, create=True))
        s.put(self.secondary_settings, s.encoded({'second_root_untouched': True, 'camera': {s.KEY: False}}))
        self.registry = self.home / '.config/openvr/openvrpaths.vrpath'
        s.put(self.registry, s.encoded({'config': [str(self.settings.parent), str(self.secondary_settings.parent)], 'runtime': ['/opt/steamvr']}))
        self.registry.chmod(0o755)
        os.close(s.directory(self.setup.config, create=True, private=True))

    def tearDown(self):
        self.temp.cleanup()

    def test_mode666_changed_inode_after_read_is_not_adopted(self):
        self.settings.chmod(0o666)
        expected = self.settings.read_bytes()
        foreign = s.encoded({'camera': {'other': 'FOREIGN'}})
        actual_read = s.read
        swapped = False
        def replace_after_read(path, *args, **kwargs):
            nonlocal swapped
            result = actual_read(path, *args, **kwargs)
            if path == self.settings and kwargs.get('with_stat') and not swapped:
                swapped = True
                replacement = path.with_name('foreign.vrsettings')
                replacement.write_bytes(foreign)
                replacement.chmod(0o666)
                os.replace(replacement, path)
            return result
        s.read = replace_after_read
        try:
            with self.assertRaises(s.SafetyError):
                s.put(self.settings, b'NOT ALLOWED', expected, writable_data=True)
        finally:
            s.read = actual_read
        self.assertTrue(swapped)
        self.assertEqual(self.settings.read_bytes(), foreign)

    def test_mode666_settings_symlink_hardlink_and_nonboolean_stay_refused(self):
        raw = self.settings.read_bytes()
        original = self.settings.with_name('original.vrsettings')
        self.settings.rename(original)
        self.settings.symlink_to(original)
        with self.assertRaises(OSError):
            self.setup._camera_state()
        self.settings.unlink()
        os.link(original, self.settings)
        with self.assertRaises(s.SafetyError):
            self.setup._camera_state()
        self.settings.unlink()
        original.rename(self.settings)
        self.settings.write_bytes(s.encoded({'camera': {s.KEY: 1}}))
        self.settings.chmod(0o666)
        with self.assertRaises(s.SafetyError):
            self.setup._camera_state()
        self.assertNotEqual(self.settings.read_bytes(), raw)
        self.assertFalse(self.setup.camera_receipt.exists())

    def test_first_history_missing_data_does_not_fall_back_to_second(self):
        second = self.secondary_settings.read_bytes()
        self.settings.unlink()
        self.assertEqual(self.setup._settings(), self.settings)
        with self.assertRaises(FileNotFoundError):
            self.setup._camera_state()
        self.assertEqual(self.secondary_settings.read_bytes(), second)
        self.assertFalse(self.setup.camera_receipt.exists())

    def test_config_environment_has_exact_nonempty_priority(self):
        alternate = self.root / 'owned-config'
        alternate.mkdir(mode=0o700)
        target = alternate / 'steamvr.vrsettings'
        s.put(target, s.encoded({'camera': {'other': 'alternate'}}))
        target.chmod(0o666)
        first, second = self.settings.read_bytes(), self.secondary_settings.read_bytes()
        with mock.patch.dict(os.environ, {'VR_CONFIG_PATH': str(alternate)}):
            self.assertEqual(self.setup._settings(), target)
            s.put(self.setup.camera_receipt, s.encoded({'schema': s.CAMERA, 'settings': str(target),
                'previous': {'present': False, 'value': None}, 'write_intent': True}))
            target.write_bytes(s.encoded({'camera': {'other': 'alternate', s.KEY: True}}))
            self.assertTrue(self.setup._camera(False)['restored'])
        self.assertEqual(self.setup._settings(), self.settings)  # Empty env is ignored, not trimmed/guessed.
        self.assertEqual(self.settings.read_bytes(), first)
        self.assertEqual(self.secondary_settings.read_bytes(), second)

    def test_registry_environment_selection_matches_public_openvr_priority(self):
        alternate = self.root / 'alternate-config'
        alternate.mkdir(mode=0o700)
        registry = self.root / 'registered.vrpath'
        s.put(registry, s.encoded({'config': [str(alternate), str(self.settings.parent)]}))
        xdg = self.root / 'xdg'
        os.close(s.directory(xdg / 'openvr', create=True))
        s.put(xdg / 'openvr/openvrpaths.vrpath', s.encoded({'config': [str(self.settings.parent)]}))
        with mock.patch.dict(os.environ, {'VR_PATHREG_OVERRIDE': str(registry), 'XDG_CONFIG_HOME': str(xdg)}):
            self.assertEqual(self.setup._settings(), alternate / 'steamvr.vrsettings')
        with mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': str(xdg)}):
            self.assertEqual(self.setup._settings(), self.settings)
        with mock.patch.dict(os.environ, {'VR_PATHREG_OVERRIDE': 'relative.vrpath'}):
            with self.assertRaises(s.SafetyError):
                self.setup._settings()

    def test_missing_malformed_and_foreign_registered_targets_refuse(self):
        original = s.read(self.registry)
        for configs in (None, [], '', [None], [''], ['relative'], ['/tmp/../escape'], [str(self.settings.parent)] * 33):
            with self.subTest(configs=configs):
                s.put(self.registry, s.encoded({'config': configs}), s.read(self.registry))
                with self.assertRaises((s.SafetyError, OSError)):
                    self.setup._settings()
        s.put(self.registry, original, s.read(self.registry))
        self.registry.unlink()
        with self.assertRaises(FileNotFoundError):
            self.setup._settings()  # Never guess the still-existing second Steam config.
        s.put(self.registry, original)
        self.settings.parent.chmod(0o777)
        with self.assertRaises(s.SafetyError):
            self.setup._settings()
        self.settings.parent.chmod(0o700)
        link = self.root / 'foreign-config-link'
        link.symlink_to(self.settings.parent)
        s.put(self.registry, s.encoded({'config': [str(link)]}), s.read(self.registry))
        with self.assertRaises(OSError):
            self.setup._settings()
        self.assertFalse(self.setup.camera_receipt.exists())

    def test_cli_no_install_and_no_implicit_restart(self):
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/native_passthrough_setup.py'), 'status', '--home', str(self.home)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)['installed'])
        source = (ROOT / 'tools/native_passthrough_setup.py').read_text()
        self.assertNotIn("'restart'", source)
        self.assertNotIn("'sudo'", source)


    def legacy_fixture(self, *, published=False, baseline=None):
        """Seed historical receipt bytes, never execute a retired installer."""
        files = {self.setup.tools / 'native_passthrough_setup.py': b'# old owned helper',
                 self.setup.dropin: b'[Service]\n# historical owned hook\n'}
        files.update({self.setup.package / name: path.read_bytes()
                      for path in self.package.rglob('*') if path.is_file()
                      for name in [str(path.relative_to(self.package))]})
        for path, raw in files.items():
            os.close(s.directory(path.parent, create=True, private=True))
            s.put(path, raw)
        receipt = {'schema': s.SCHEMA, 'home': str(self.home), 'data': str(self.setup.data),
                   'manifest_link': str(self.setup.target),
                   'package_sha256': s.digest(s.read(self.setup.package / 'package.json')),
                   'files': {str(path): s.digest(raw) for path, raw in files.items()}}
        s.put(self.setup.receipt, s.encoded(receipt))
        self.setup._enabled(True)
        if published:
            os.close(s.directory(self.runtime, create=True, private=True))
            manifest, config = b'owned historical manifest', b'owned historical session'
            s.put(self.setup.target, manifest)
            s.put(self.setup.session, config)
            s.put(self.setup.inflight, s.encoded({'schema': 'sfnp-launch-v1',
                'directory': str(self.setup.runs / ('run-' + 'a'*32)), 'capture_id': 'a'*32,
                'manifest_sha256': s.digest(manifest), 'config_sha256': s.digest(config)}))
            os.close(s.directory(self.setup.manifest.parent, create=True))
            self.setup.manifest.symlink_to(self.setup.target)
            s.put(self.setup.camera_receipt, s.encoded({'schema': s.CAMERA,
                'settings': str(self.settings), 'previous': {'present': baseline is not None, 'value': baseline},
                'write_intent': baseline is not True}))
            value = s.load(self.settings); value['camera'][s.KEY] = True
            s.put(self.settings, s.encoded(value), s.read(self.settings))
        return receipt

    def test_retired_install_enable_and_automatic_build_refuse_before_any_mutation(self):
        before = {str(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        with mock.patch.object(self.setup, '_verify', side_effect=AssertionError('No old generator or runtime')):
            for action in (lambda: self.setup.install(self.package), self.setup.install_automatic,
                           self.setup.enable, lambda: self.setup._camera(True)):
                with self.assertRaisesRegex(s.SafetyError, '(retired|Retired)'):
                    action()
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()})
        self.assertEqual(self.unit.native_calls, [])

    def test_stale_service_prepare_only_revokes_owned_publication_and_restores_camera(self):
        self.legacy_fixture(published=True)
        with mock.patch.object(self.setup, '_verify', side_effect=AssertionError('Cold removal needs no runtime grant')):
            result = self.setup.prepare()
        self.assertEqual(result['reason'], 'retired_transport')
        self.assertFalse(result['enabled']); self.assertTrue(result['camera']['restored'])
        for path in (self.setup.target, self.setup.session, self.setup.manifest):
            self.assertFalse(s.exists(path))
        self.assertNotIn(s.KEY, s.load(self.settings)['camera'])
        self.assertEqual(self.unit.native_calls, [])
        self.assertFalse(self.unit.live)

    def test_uninstalled_old_hook_is_noop_and_never_creates_runtime_files(self):
        self.assertEqual(self.setup.prepare()['reason'], 'retired_transport')
        self.assertFalse(self.setup.target.exists()); self.assertFalse(self.setup.receipt.exists())
        self.assertEqual(self.unit.native_calls, [])

    def test_uninstall_removes_exact_owned_files_preserving_other_hooks_and_settings(self):
        receipt = self.legacy_fixture(published=True)
        foreign = self.setup.dropin.with_name('10-other.conf'); foreign.write_bytes(b'foreign hook')
        self.assertEqual(self.setup.uninstall()['reason'], 'uninstalled')
        self.assertTrue(all(not Path(name).exists() for name in receipt['files']))
        self.assertFalse(self.setup.receipt.exists()); self.assertEqual(foreign.read_bytes(), b'foreign hook')
        self.assertEqual(s.load(self.settings), {'unrelated': {'keep': 42}, 'camera': {'other': 'keep'}})
        self.assertFalse(self.unit.live)

    def test_changed_owned_payload_is_preserved_and_receipt_retained(self):
        self.legacy_fixture(published=True)
        image = self.setup.package / s.LIBRARY
        s.put(image, b'foreign replacement', s.read(image))
        with self.assertRaisesRegex(s.SafetyError, 'changed|mismatch'):
            self.setup.uninstall()
        self.assertEqual(image.read_bytes(), b'foreign replacement')
        self.assertTrue(self.setup.receipt.exists())
        self.assertFalse(s.exists(self.setup.manifest))

    def test_each_historical_camera_baseline_and_writable_data_mode_restore(self):
        for baseline in (None, False, True):
            with self.subTest(baseline=baseline):
                if self.setup.camera_receipt.exists(): self.setup.camera_receipt.unlink()
                self.settings.chmod(0o600)
                value = s.load(self.settings); value['camera'][s.KEY] = True
                s.put(self.settings, s.encoded(value), s.read(self.settings)); self.settings.chmod(0o666)
                s.put(self.setup.camera_receipt, s.encoded({'schema': s.CAMERA, 'settings': str(self.settings),
                    'previous': {'present': baseline is not None, 'value': baseline}, 'write_intent': baseline is not True}))
                self.assertTrue(self.setup._restore()['restored'])
                restored = s.json_bytes(self.settings.read_bytes())
                self.assertEqual(restored['camera'].get(s.KEY), baseline)
                self.assertEqual(restored['camera']['other'], 'keep')
                self.assertEqual(restored['unrelated'], {'keep': 42})
                self.assertEqual(self.settings.stat().st_mode & 0o777, 0o666)

    def test_unknown_live_runtime_never_called_and_owned_activation_still_revoked(self):
        self.legacy_fixture(published=True); self.unit.live = True; self.compatible = False
        result = self.setup.disable(True)
        self.assertFalse(result['camera']['restored'])
        self.assertEqual(self.unit.native_calls, []); self.assertFalse(s.exists(self.setup.manifest))
        self.assertTrue(self.setup.camera_receipt.exists())
        self.assertEqual(self.setup.uninstall()['reason'], 'uninstall_pending')
        self.assertTrue(self.unit.live)

    def test_live_uninstall_retains_restore_helper_until_normal_stop(self):
        self.legacy_fixture(published=True); self.unit.live = True
        self.unit.persist = False
        result = self.setup.uninstall()
        self.assertEqual(result['reason'], 'uninstall_pending')
        self.assertTrue(self.setup.dropin.exists()); self.assertTrue(self.setup.receipt.exists())
        self.assertTrue(self.unit.live); self.assertFalse(s.exists(self.setup.manifest))
        self.unit.live = False
        self.assertEqual(self.setup.uninstall()['reason'], 'uninstalled')
        self.assertNotIn(s.KEY, s.load(self.settings)['camera'])

    def test_foreign_manifest_and_camera_edits_are_preserved(self):
        self.legacy_fixture(published=True)
        self.setup.manifest.unlink(); self.setup.manifest.write_bytes(b'foreign manifest')
        with self.assertRaisesRegex(s.SafetyError, 'foreign'):
            self.setup.disable()
        self.assertEqual(self.setup.manifest.read_bytes(), b'foreign manifest')
        value = s.load(self.settings); value['camera'][s.KEY] = False
        s.put(self.settings, s.encoded(value), s.read(self.settings))
        self.assertFalse(self.setup._restore()['restored'])
        self.assertFalse(s.load(self.settings)['camera'][s.KEY])

    def test_retired_legacy_rpc_cannot_install_or_pair(self):
        engine_path = ROOT / 'setup/steam_link_setup.py'
        spec = importlib.util.spec_from_file_location('retired_rpc_owner', engine_path)
        owner = importlib.util.module_from_spec(spec); spec.loader.exec_module(owner)
        remote = {'__name__': 'retired_remote_test'}; exec(owner.REMOTE_AGENT, remote)
        for action in ('install', 'update', 'repair', 'uninstall'):
            with self.assertRaisesRegex(ValueError, 'retired'):
                remote['run']({'command': 'apply', 'action': action}, home=self.home)
        self.assertFalse((self.home / '.config/steam-link-native-passthrough/pairings').exists())


if __name__ == '__main__':
    unittest.main()
