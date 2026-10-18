#!/usr/bin/env python3
"""Owned fake-file controls; no Valve body, compiler execution or Frame action."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_prepare', ROOT / 'tools/prepare_native_release.py')
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sfnp-owned-test-')
        self.base = Path(self.temp.name)
        self.package = self.base / 'package'; self.package.mkdir(mode=0o700)
        self.profile = copy.deepcopy(p.known_compatibility())
        self.files = {}
        for role, item in self.profile['runtime_files'].items():
            raw = ('owned fixture ' + role).encode()
            path = self.base / role; path.write_bytes(raw); self.files[role] = path
            item.update(bytes=len(raw), sha256=p.sha(raw))
        self.candidate = b'OWNED SYNTHETIC PLACEHOLDER; NEVER VALVE SPIR-V'
        self.profile['candidate'] = {'bytes': len(self.candidate), 'sha256': p.sha(self.candidate)}
        self.compat = p.json_bytes(self.profile)
        self.patches = [mock.patch.object(p, 'known_compatibility', return_value=self.profile),
                        mock.patch.object(p, 'COMPATIBILITY_SHA256', p.sha(self.compat))]
        for patch in self.patches: patch.start()
        elf = bytearray(64); elf[:7] = b'\x7fELF\x02\x01\x01'; struct.pack_into('<HH', elf, 16, 3, 183)
        self.artifacts = {p.LIBRARY: bytes(elf) + p.BUILD_MARKER, p.CANDIDATE: self.candidate,
            'compatibility.json': self.compat, 'source-manifest.json': p.json_bytes(p.source_snapshot())}
        self.artifacts.update({name: (ROOT / name).read_bytes() for name in p.HELPERS})
        self.manifest = {'schema': p.SCHEMA, 'profile': self.profile['profile'],
            'build_marker': p.BUILD_MARKER[:-1].decode(), 'source_snapshot_sha256': p.sha(self.artifacts['source-manifest.json']),
            'files': {name: {'bytes': len(raw), 'sha256': p.sha(raw)} for name, raw in self.artifacts.items()}}
        for name, raw in self.artifacts.items(): self.write(name, raw)
        self.save()

    def tearDown(self):
        for patch in reversed(self.patches): patch.stop()
        self.temp.cleanup()

    def write(self, name, raw):
        target = self.package / name; target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        target.write_bytes(raw); target.chmod(0o600)

    def save(self):
        self.write('package.json', p.json_bytes(self.manifest))

    def changed(self, name, raw):
        self.write(name, raw); self.manifest['files'][name] = {'bytes': len(raw), 'sha256': p.sha(raw)}; self.save()

    def verify(self):
        return p.verify_package(self.package, self.files)

    def test_owned_fixture_valid(self):
        self.assertEqual(self.verify()['profile'], self.profile['profile'])

    def test_each_runtime_role_change_refuses(self):
        for role, file in self.files.items():
            before = file.read_bytes(); file.write_bytes(b'X' * len(before))
            with self.subTest(role=role), self.assertRaisesRegex(p.SafetyError, role): self.verify()
            file.write_bytes(before)

    def test_missing_runtime_role_refuses(self):
        del self.files['driver']
        with self.assertRaisesRegex(p.SafetyError, 'role mapping'): self.verify()

    def test_system_symlink_resolves_only_exact_bytes(self):
        link = self.base / 'system-alias'; link.symlink_to(self.files['driver'])
        self.files['driver'] = link; self.verify()
        link.unlink(); link.symlink_to(self.files['compositor'])
        with self.assertRaises(p.SafetyError): self.verify()

    def test_forged_compatibility_not_self_authorized(self):
        forged = copy.deepcopy(self.profile); forged['runtime_files']['driver']['sha256'] = '0' * 64
        self.changed('compatibility.json', p.json_bytes(forged))
        with self.assertRaisesRegex(p.SafetyError, 'compatibility'): self.verify()

    def test_candidate_manifest_update_does_not_authorize_new_code(self):
        self.changed(p.CANDIDATE, b'X' * len(self.candidate))
        with self.assertRaisesRegex(p.SafetyError, 'candidate'): self.verify()

    def test_wrong_elf_or_missing_release_marker(self):
        original = self.artifacts[p.LIBRARY]
        for value in (original.replace(p.BUILD_MARKER, b'X' * len(p.BUILD_MARKER)),
                      original[:18] + struct.pack('<H', 62) + original[20:]):
            self.changed(p.LIBRARY, value)
            with self.assertRaises(p.SafetyError): self.verify()

    def test_helper_self_updated_manifest_is_not_authority(self):
        name = p.HELPERS[0]; raw = self.artifacts[name] + b'\n# foreign modification\n'
        self.changed(name, raw)
        source = p.source_snapshot(); source[name] = p.sha(raw)
        self.changed('source-manifest.json', p.json_bytes(source))
        self.manifest['source_snapshot_sha256'] = p.sha(p.json_bytes(source)); self.save()
        with self.assertRaisesRegex(p.SafetyError, 'helper differs'): self.verify()

    def test_source_snapshot_cannot_be_invented(self):
        source = p.source_snapshot(); source['layer/pixel_snapshot_layer.cpp'] = '0' * 64
        raw = p.json_bytes(source); self.changed('source-manifest.json', raw)
        self.manifest['source_snapshot_sha256'] = p.sha(raw); self.save()
        with self.assertRaisesRegex(p.SafetyError, 'source snapshot differs'): self.verify()

    def test_all_frozen_source_profiles_are_bound(self):
        source = p.source_snapshot()
        for revision in (1, 2, 3):
            name = 'docs/protocols/scene-contribution-atlas-v%d.json' % revision
            self.assertEqual(source[name], p.sha((ROOT / name).read_bytes()))
        # A self-updated package manifest cannot omit the new protocol input.
        del source['docs/protocols/scene-contribution-atlas-v3.json']
        raw = p.json_bytes(source); self.changed('source-manifest.json', raw)
        self.manifest['source_snapshot_sha256'] = p.sha(raw); self.save()
        with self.assertRaisesRegex(p.SafetyError, 'source snapshot differs'): self.verify()

    def test_package_file_symlink_hardlink_and_mode_refuse(self):
        target = self.package / p.CANDIDATE; target.unlink(); target.symlink_to(self.files['driver'])
        with self.assertRaises(OSError): self.verify()
        target.unlink(); target.write_bytes(self.candidate); target.chmod(0o600)
        os.link(target, self.base / 'duplicate')
        with self.assertRaises(p.SafetyError): self.verify()
        (self.base / 'duplicate').unlink(); target.chmod(0o644)
        with self.assertRaises(p.SafetyError): self.verify()

    def test_package_directory_symlink_and_mode_refuse(self):
        alias = self.base / 'alias'; alias.symlink_to(self.package, target_is_directory=True)
        with self.assertRaises(p.SafetyError): p.verify_package(alias, self.files)
        self.package.chmod(0o755)
        with self.assertRaises(p.SafetyError): self.verify()

    def test_package_parent_symlink_refuses(self):
        tools = self.package / 'tools'; moved = self.base / 'borrowed-tools'; tools.rename(moved)
        tools.symlink_to(moved, target_is_directory=True)
        with self.assertRaisesRegex(p.SafetyError, 'subdirectory'): self.verify()

    def test_duplicate_or_extra_manifest_keys_refuse(self):
        self.write('package.json', b'{"schema":1,"schema":2}')
        with self.assertRaisesRegex(p.SafetyError, 'duplicate'): self.verify()
        self.manifest['extra'] = True; self.save()
        with self.assertRaisesRegex(p.SafetyError, 'schema'): self.verify()

    def test_wrong_metadata_container_is_a_controlled_refusal(self):
        self.write('package.json', b'[]')
        with self.assertRaises(p.SafetyError): self.verify()

    def test_initial_oversize_does_not_read_entire_file(self):
        path = self.base / 'large'; path.write_bytes(b'x' * 1000)
        with mock.patch.object(p.os, 'read', side_effect=AssertionError('must stat-refuse first')):
            with self.assertRaises(p.SafetyError): p.read_regular(path, 10)

    def test_growth_during_read_is_bounded(self):
        path = self.base / 'growing'; path.write_bytes(b'1234')
        real = p.os.read; calls = []
        def growing(fd, size):
            calls.append(size)
            if len(calls) == 1:
                with path.open('ab') as stream: stream.write(b'X' * 10000)
            return real(fd, size)
        with mock.patch.object(p.os, 'read', side_effect=growing):
            with self.assertRaises(p.SafetyError): p.read_regular(path, 4)
        self.assertLessEqual(sum(calls), 5)

    def test_prepare_non_arm_and_missing_tools_preserve_existing_output(self):
        before = {path: path.read_bytes() for path in self.files.values()}
        with mock.patch.object(p.platform, 'machine', return_value='x86_64'):
            with self.assertRaisesRegex(p.SafetyError, 'AArch64'): p.prepare(self.package)
        with mock.patch.object(p.platform, 'machine', return_value='aarch64'), mock.patch.object(p.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(p.SafetyError, 'tool missing'): p.prepare(self.package)
        self.assertEqual(before, {path: path.read_bytes() for path in self.files.values()})

    def preparation(self, *, wrong_candidate=False, changed_source=False):
        profile_path = self.base / 'reviewed-fixture-profile.json'; profile_path.write_bytes(self.compat)
        snapshot = p.source_snapshot()
        def robust(original, enabled, descriptor_count):
            self.assertEqual(original, self.files['fragment'].read_bytes())
            self.assertIs(enabled, True); self.assertEqual(descriptor_count, 1)
            return (b'wrong' if wrong_candidate else self.candidate), {'owned_fixture_only': True}
        module = types.SimpleNamespace(transform_robust=robust)
        calls = []
        def command(argv, log, timeout=600):
            calls.append(argv); Path(log).touch()
            if argv[:2] == ['cmake', '--build']:
                build = Path(argv[2]); build.mkdir(parents=True)
                (build / p.LIBRARY).write_bytes(self.artifacts[p.LIBRARY])
        snapshots = [snapshot, {}] if changed_source else None
        with mock.patch.object(p.platform, 'machine', return_value='aarch64'), \
             mock.patch.object(p.shutil, 'which', return_value='/owned/tool'), \
             mock.patch.object(p, 'COMPATIBILITY', profile_path), \
             mock.patch.object(p, 'runtime_files', return_value=self.files), \
             mock.patch.object(p, 'source_snapshot', side_effect=snapshots, return_value=snapshot), \
             mock.patch.object(p, 'run_command', side_effect=command), \
             mock.patch.dict(sys.modules, {'native_fragment_scene_presenter': module}):
            value = p.prepare(self.base / 'new-package')
        return value, calls

    def test_preparation_builds_then_validates_before_publication(self):
        value, commands = self.preparation()
        self.assertEqual(value['schema'], p.SCHEMA)
        self.assertTrue((self.base / 'new-package/package.json').is_file())
        self.assertEqual([x[0] for x in commands], ['cmake', 'cmake', 'spirv-val'])
        runs = list(self.base.glob('new-package.preparation-*')); self.assertEqual(len(runs), 1)
        self.assertFalse((runs[0] / 'cache').exists())
        self.assertTrue(json.loads((runs[0] / 'result.json').read_text())['success'])
        self.assertTrue((runs[0] / 'sources/tools/prepare_native_release.py').is_file())

    def test_wrong_generation_or_changed_source_never_publishes(self):
        for kwargs in ({'wrong_candidate': True}, {'changed_source': True}):
            with self.subTest(kwargs=kwargs), self.assertRaises(p.SafetyError): self.preparation(**kwargs)
            self.assertFalse((self.base / 'new-package').exists())
        runs = list(self.base.glob('new-package.preparation-*')); self.assertEqual(len(runs), 2)
        for run in runs:
            self.assertFalse((run / 'cache').exists())
            self.assertFalse(json.loads((run / 'result.json').read_text())['success'])
            self.assertTrue((run / 'source-manifest.json').is_file())

    def test_atomic_publication_never_overwrites_existing_directory(self):
        stage = self.base / 'stage'; stage.mkdir(); (stage / 'owned').write_text('original')
        output = self.base / 'output'; output.mkdir()
        with self.assertRaises(OSError): p.publish_directory(stage, output)
        self.assertEqual(list(output.iterdir()), [])
        self.assertEqual((stage / 'owned').read_text(), 'original')
        output.rmdir(); p.publish_directory(stage, output)
        self.assertEqual((output / 'owned').read_text(), 'original')


class RealProfileTests(unittest.TestCase):
    def test_exact_reviewed_public_profile(self):
        profile = p.known_compatibility()
        self.assertEqual(len(profile['runtime_files']), 10)
        self.assertEqual(profile['runtime_files']['fragment']['bytes'], 6620)
        self.assertEqual(profile['candidate']['bytes'], 31384)
        self.assertEqual(profile['candidate']['sha256'], '44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49')
        paths = p.runtime_files(home=Path('/home/owned'))
        self.assertEqual(paths['vulkan_loader'], Path('/home/owned/.local/share/Steam/steamrtarm64/libvulkan.so.1'))
        with mock.patch.object(p, 'COMPATIBILITY_SHA256', '0' * 64):
            with self.assertRaisesRegex(p.SafetyError, 'manifest pin'): p.known_compatibility()

    def test_release_config_and_process_pin_cpp(self):
        with tempfile.TemporaryDirectory(prefix='sfnp-config-test-') as directory:
            base = Path(directory); source = base / 'test.cpp'
            source.write_text('#include "layer/pixel_snapshot_io.h"\n#include <iostream>\nint main(){std::cout<<frame_pixel_io::defaultConfigPath();'
                              '\n#ifdef FRAME_PIXEL_RELEASE_RECEIVER\nif(frame_pixel_io::releaseProcessMatches())return 7;\n#endif\n}\n')
            for release in (False, True):
                binary = base / ('release' if release else 'diagnostic')
                command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-I', str(ROOT), str(source), '-o', str(binary)]
                if release: command.insert(1, '-DFRAME_PIXEL_RELEASE_RECEIVER=1')
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                env = dict(os.environ, HOME='/home/owned', GHVR_PIXEL_SNAPSHOT_CONFIG='/foreign/ignored')
                result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, 'host executable must fail the native process pin')
                expected = '/home/owned/.local/share/SteamFrameNativePassthrough/session.conf' if release else '/home/steamos/.local/share/GloomhavenVR/SteamLinkMaskProbe/session.conf'
                self.assertEqual(result.stdout, expected)
                if release:
                    env['HOME'] = 'relative'; result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.stdout, '')
                    env.pop('HOME'); result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
