#!/usr/bin/env python3
"""Run the actual online bootstrap parsers against local repository, release and ZIP fixtures."""
from __future__ import annotations
import argparse
import hashlib
import io
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'get.sh').read_text().split("<<'SFNP_BOOTSTRAP_PY'\n", 1)[1].rsplit('\nSFNP_BOOTSTRAP_PY', 1)[0]
bootstrap = types.ModuleType('sfnp_bootstrap_under_test')
exec(compile(source, str(ROOT / 'get.sh'), 'exec'), bootstrap.__dict__)
spec = importlib.util.spec_from_file_location('sfnp_packaging_under_test', ROOT / 'tools/package_stream_link_setup.py')
packaging = importlib.util.module_from_spec(spec); spec.loader.exec_module(packaging)
spec = importlib.util.spec_from_file_location('sfnp_bridge_package_fixtures', ROOT / 'tests/test_package_stream_bridge.py')
bridge_fixtures = importlib.util.module_from_spec(spec); spec.loader.exec_module(bridge_fixtures)
POWERSHELL = None
OUT = None


def legacy_contents():
    return {'steam-link-bundle.json': json.dumps({'schema': 'sfnp-steam-link-setup-v1', 'readiness': 'release'}).encode(),
            'setup/steam_link_setup.py': b'# Non-executable fixture content.\n',
            'setup/runtime/python/python.exe': b'FIXTURE ONLY: NOT A REAL EXECUTABLE\n'}


def contents():
    return {'stream-bridge-bundle.json': json.dumps({'schema':'sfnp-stream-bridge-bundle-v1', 'readiness':'release'}).encode(),
            'setup/stream_bridge_install.py': b'# Non-executable paired installer fixture.\n',
            'setup/steam_link_setup.py': b'# Non-executable paired setup fixture.\n',
            'setup/runtime/python/python.exe': b'FIXTURE ONLY: NOT A REAL EXECUTABLE\n'}


def inspection_contents():
    return {'setup-inspection.json': json.dumps({'schema': 'sfnp-setup-inspection-v1',
            'expected_pc': {}, 'runtime_files': {}}).encode(),
            'setup/steam_link_setup.py': b'# Non-executable inspection fixture.\n',
            'setup/runtime/python/python.exe': b'FIXTURE ONLY: NOT A REAL EXECUTABLE\n'}


def manager_contents():
    files = inspection_contents()
    files['setup/frame_native_uninstall.py'] = b'# Non-executable removal helper fixture.\n'
    files['setup-maintenance.json'] = json.dumps({'schema':'sfnp-setup-maintenance-v1', 'helpers':{}}).encode()
    return files


def archive(files=None, *, mutate=None, extra=None, inspection=False, maintenance=False, legacy=False):
    files = (manager_contents() if maintenance else inspection_contents() if inspection else legacy_contents() if legacy else contents()) if files is None else files
    manifest = {'schema': 'sfnp-bootstrap-manager-v1' if maintenance else 'sfnp-bootstrap-inspection-v1' if inspection else 'sfnp-bootstrap-package-v1' if legacy else 'sfnp-bootstrap-stream-bridge-v1',
                'readiness': 'maintenance' if maintenance else 'inspection-only' if inspection else 'release',
                'windows_python': 'setup/runtime/python/python.exe',
                'files': {name: {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()} for name, raw in files.items()}}
    if mutate:
        mutate(manifest)
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as target:
        for name, raw in files.items():
            target.writestr(name, raw)
        target.writestr('bootstrap-package.json', json.dumps(manifest))
        if extra:
            target.writestr(*extra)
    return stream.getvalue()


def bridge_archive(*, release=True):
    # The actual bridge assembler validates its complete owned-source profile,
    # synthetic PE/ELF/dependency artifacts and explicit software-only receipt.
    # Reuse those fixtures without running unrelated packaging test methods.
    fixture = bridge_fixtures.Tests()
    fixture.setUp()
    try:
        kwargs = {'release': True, 'validation_receipts': [fixture.validation()]} if release else {}
        raw, receipt = fixture.build(**kwargs)
        assert receipt['hardware_verified'] is False
        return raw
    finally:
        fixture.doCleanups()


def repack(raw, updates):
    """Keep complete file pins while testing independently wrong inner contracts."""
    with zipfile.ZipFile(io.BytesIO(raw)) as package:
        files = {name: package.read(name) for name in package.namelist()}
    manifest = json.loads(files.pop('bootstrap-package.json'))
    files.update(updates)
    manifest['files'] = {name: {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
                         for name, value in files.items()}
    result = io.BytesIO()
    with zipfile.ZipFile(result, 'w', zipfile.ZIP_DEFLATED) as package:
        for name, value in files.items(): package.writestr(name, value)
        package.writestr('bootstrap-package.json', json.dumps(manifest))
    return result.getvalue()


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=OUT)
        self.path = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def release(self, inspection=False, maintenance=False, bridge_preview=False):
        bridge_preview = not inspection and not maintenance
        tag = bootstrap.BRIDGE_TAG if bridge_preview else bootstrap.MANAGER_TAG if maintenance else bootstrap.INSPECTION_TAG if inspection else 'v1.0.0'
        asset_name = bootstrap.BRIDGE_ASSET if bridge_preview else bootstrap.MANAGER_ASSET if maintenance else bootstrap.INSPECTION_ASSET if inspection else bootstrap.BRIDGE_ASSET
        return {'draft': False, 'prerelease': inspection or maintenance or bridge_preview, 'tag_name': tag,
                'assets': [{'name': asset_name, 'state': 'uploaded', 'size': 512,
                            'digest': 'sha256:' + 'a' * 64,
                            'browser_download_url': 'https://github.com/' + bootstrap.REPO + '/releases/download/' + tag + '/' + asset_name}]}

    def extract(self, raw, inspection=False, maintenance=False, bridge_preview=False):
        target = self.path / 'package'; target.mkdir()
        zipped = self.path / 'download.zip'; zipped.write_bytes(raw)
        bootstrap.extract(zipped, target, inspection, maintenance, bridge_preview)
        return target

    def test_repository_payload_is_immutable_and_matches_powershell(self):
        import re
        asset = bootstrap.repository_asset()
        self.assertRegex(bootstrap.REPOSITORY_COMMIT, r'^[0-9a-f]{40}$')
        self.assertEqual(asset['browser_download_url'], 'https://raw.githubusercontent.com/' +
                         bootstrap.REPO + '/' + bootstrap.REPOSITORY_COMMIT + '/' + bootstrap.REPOSITORY_ASSET)
        self.assertGreater(asset['size'], 0); self.assertLessEqual(asset['size'], bootstrap.MAX_ZIP)
        self.assertRegex(asset['digest'], r'^sha256:[0-9a-f]{64}$')
        powershell = (ROOT / 'get.ps1').read_text()
        for name, value in [('Commit', bootstrap.REPOSITORY_COMMIT), ('Asset', bootstrap.REPOSITORY_ASSET),
                            ('Bytes', bootstrap.REPOSITORY_BYTES), ('Sha256', bootstrap.REPOSITORY_SHA256)]:
            expected = str(value) if type(value) is int else "'" + value + "'"
            self.assertIn('$sfnpRepository' + name + ' = ' + expected, powershell)
        self.assertIn("'raw.githubusercontent.com'", powershell)
        self.assertRegex(powershell, r'if \(\$sfnpBridgePreview\) \{\s*\$asset = Get-SfnpRepositoryAsset\s*\} else \{\s*\$releaseBytes')

    def test_repository_payload_matches_checked_in_bytes(self):
        raw = (ROOT / bootstrap.REPOSITORY_ASSET).read_bytes()
        self.assertEqual(len(raw), bootstrap.REPOSITORY_BYTES)
        self.assertEqual(hashlib.sha256(raw).hexdigest(), bootstrap.REPOSITORY_SHA256)

    def test_exact_public_asset(self):
        self.assertEqual(bootstrap.select_asset(self.release())['size'], 512)

    def test_default_rejects_retired_frame_only_asset_and_archive(self):
        retired = self.release()
        retired.update(prerelease=False, tag_name='v1.0.0')
        retired['assets'][0]['name'] = 'steam-link-native-passthrough-setup.zip'
        with self.assertRaises(ValueError): bootstrap.select_asset(retired)
        for explicit in (False,True):
            with tempfile.TemporaryDirectory(dir=self.path) as temporary:
                target=Path(temporary)/'target';target.mkdir()
                zipped=Path(temporary)/'old.zip';zipped.write_bytes(archive(legacy=True))
                with self.assertRaises(ValueError): bootstrap.extract(zipped,target,bridge_preview=explicit)
                self.assertEqual(list(target.iterdir()),[])

    def test_preview_wrappers_pin_reviewed_core_and_select_manager(self):
        for name, source, flag in [('get-preview.ps1', 'get.ps1', '-BridgePreview'),
                                   ('get-preview.sh', 'get.sh', '--bridge-preview')]:
            text = (ROOT / name).read_text()
            digest = hashlib.sha256((ROOT / source).read_bytes()).hexdigest()
            self.assertIn(digest, text)
            import re
            self.assertRegex(text, r'/[0-9a-f]{40}/' + re.escape(source))
            self.assertIn(flag, text)

    def test_explicit_inspection_release_is_separate_from_stable(self):
        release = self.release(True)
        self.assertEqual(bootstrap.select_asset(release, True)['name'], bootstrap.INSPECTION_ASSET)
        for candidate, mode in [(release, False), (self.release(), True)]:
            with self.assertRaises(ValueError): bootstrap.select_asset(candidate, mode)
        for field, value in [('draft', True), ('prerelease', False), ('tag_name', 'another-inspection')]:
            candidate = self.release(True); candidate[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): bootstrap.select_asset(candidate, True)
        candidate = self.release(True); candidate['assets'][0]['name'] = bootstrap.BRIDGE_ASSET
        with self.assertRaises(ValueError): bootstrap.select_asset(candidate, True)

    def test_inspection_archive_has_no_installation_payload(self):
        files = inspection_contents(); files['LICENSE'] = b'MIT attribution fixture'
        target = self.extract(archive(files, inspection=True), True)
        self.assertEqual((target / 'LICENSE').read_bytes(), files['LICENSE'])
        self.assertEqual((target / 'setup-inspection.json').read_bytes(), inspection_contents()['setup-inspection.json'])
        self.assertFalse((target / 'steam-link-bundle.json').exists())

    def test_private_vendor_allowlist_and_reviewed_large_inventory(self):
        files=inspection_contents()
        files['setup/steam_link_setup_textual.py']=b'# Owned UI source fixture\n'
        files['setup/steam_link_setup_vendor.py']=b'# Owned loader source fixture\n'
        files.update({'setup/runtime/vendor/textual/fixture_'+str(i)+'_'+'x'*100+'.py':b'' for i in range(1400)})
        raw=archive(files,inspection=True)
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            self.assertGreater(z.getinfo('bootstrap-package.json').file_size,256*1024)
        target=self.extract(raw,True)
        self.assertEqual(len([p for p in (target/'setup/runtime/vendor').rglob('*') if p.is_file()]),1400)

    def test_reviewed_inventory_and_manifest_bounds_remain_enforced(self):
        with self.assertRaisesRegex(ValueError,'manifest missing'):
            self.extract(archive(mutate=lambda manifest:manifest.update(padding='x'*(1024*1024))))
        self.assertEqual(list((self.path/'package').iterdir()),[])
        (self.path/'package').rmdir()
        for count in (4096,):
            files=contents();files.update({'setup/runtime/vendor/f'+str(i)+'.py':b'' for i in range(count)})
            with self.assertRaisesRegex(ValueError,'entry bound'):
                self.extract(archive(files))

    def test_modes_cannot_accept_each_others_archives(self):
        for mode in (False, True):
            with self.subTest(inspection=mode), tempfile.TemporaryDirectory(dir=self.path) as temporary:
                target = Path(temporary) / 'target'; target.mkdir()
                zipped = Path(temporary) / 'archive.zip'; zipped.write_bytes(archive(inspection=not mode))
                with self.assertRaises(ValueError): bootstrap.extract(zipped, target, mode)
                self.assertEqual(list(target.iterdir()), [])

    def test_inspection_rejects_native_or_release_payload_before_extraction(self):
        for name in ('steam-link-bundle.json', 'pc-provider/driver.dll', 'receiver/libVkLayer.so', 'shader.spv'):
            with self.subTest(name=name), tempfile.TemporaryDirectory(dir=self.path) as temporary:
                files = inspection_contents(); files[name] = b'UNEXECUTED PAYLOAD FIXTURE'
                zipped = Path(temporary) / 'archive.zip'; zipped.write_bytes(archive(files, inspection=True))
                target = Path(temporary) / 'target'; target.mkdir()
                with self.assertRaises(ValueError): bootstrap.extract(zipped, target, True)
                self.assertEqual(list(target.iterdir()), [])

    def test_inspection_inner_profile_contract(self):
        files = inspection_contents(); files['setup-inspection.json'] = b'{"schema":"sfnp-setup-inspection-v1","readiness":"inspection-only"}'
        with self.assertRaises(ValueError): self.extract(archive(files, inspection=True), True)

    def test_only_explicit_inspection_argument(self):
        self.assertEqual(bootstrap.arguments([]), 'bridge-preview'); self.assertIs(bootstrap.arguments(['--inspect']), True)
        self.assertEqual(bootstrap.arguments(['--manage']), 'manage')
        self.assertEqual(bootstrap.arguments(['--bridge-preview']), 'bridge-preview')
        for args in (['--apply'], ['--inspect', '--apply'], ['--inspect', '--inspect'], ['--inspection'], ['--inspect','--manage'], ['--manage','--action','uninstall'],
                     ['--bridge-preview','--manage'], ['--inspect','--bridge-preview'],
                     ['--bridge-preview','--bridge-preview']):
            with self.subTest(args=args), self.assertRaises(ValueError): bootstrap.arguments(args)

    def test_incomplete_or_unexpected_release_refused(self):
        for field, value in [('draft', True), ('prerelease', False), ('tag_name', '../main'), ('assets', [])]:
            with self.subTest(field=field):
                release = self.release(); release[field] = value
                with self.assertRaises(ValueError): bootstrap.select_asset(release)
        for field, value in [('size', bootstrap.MAX_ZIP + 1), ('digest', None),
                             ('browser_download_url', 'https://example.org/unrelated.zip'), ('state', 'new')]:
            with self.subTest(asset=field):
                release = self.release(); release['assets'][0][field] = value
                with self.assertRaises(ValueError): bootstrap.select_asset(release)

    def test_verifies_and_extracts_all_files(self):
        target = self.extract(archive())
        for name, raw in contents().items(): self.assertEqual((target / name).read_bytes(), raw)

    def test_prototype_refused_before_any_executable_extraction(self):
        raw = archive(mutate=lambda value: value.update(readiness='prototype'))
        with self.assertRaises(ValueError): self.extract(raw)
        self.assertEqual(list((self.path / 'package').rglob('*')), [])

    def test_inner_prototype_bundle_never_runs(self):
        files = contents(); files['stream-bridge-bundle.json'] = b'{"schema":"sfnp-stream-bridge-bundle-v1","readiness":"prototype"}'
        with self.assertRaises(ValueError): self.extract(archive(files))

    def test_hash_mismatch_refused(self):
        raw = archive(mutate=lambda value: value['files']['setup/steam_link_setup.py'].update(sha256='0' * 64))
        with self.assertRaises(ValueError): self.extract(raw)

    def test_unsafe_names_and_links(self):
        for name in ('../outside', '/outside', 'C:/outside', 'x\\outside', 'x//outside',
                     'x/CON.txt', 'x/end.', 'x/end ', 'x/\x00bad', 'x/../bad', 'x/CONIN$', 'x/CONOUT$',
                     'x/COM\u00b9', 'x/LPT\u00b2', 'x/a|b', 'x/a?b'):
            with self.subTest(name=name), self.assertRaises(ValueError): bootstrap.safe_name(name)
        info = zipfile.ZipInfo('linked'); info.create_system = 3; info.external_attr = 0o120777 << 16
        with self.assertRaises(ValueError): self.extract(archive(extra=(info, b'../outside')))
        self.assertFalse((self.path / 'outside').exists())

    def test_extra_and_case_collisions(self):
        for extra in [('unlisted', b'new'), ('SETUP/STEAM_LINK_SETUP.PY', b'case collision')]:
            with self.subTest(extra=extra):
                out = self.path / 'package'
                if out.exists():
                    import shutil
                    shutil.rmtree(out)
                (self.path / 'download.zip').unlink(missing_ok=True)
                with self.assertRaises(ValueError): self.extract(archive(extra=extra))

    def test_redirects_are_only_https_github_asset_hosts(self):
        import urllib.request
        redirect = bootstrap.Redirect()
        request = urllib.request.Request('https://github.com/release')
        raw = bootstrap.repository_asset()['browser_download_url']
        self.assertEqual(redirect.redirect_request(request, None, 302, '', {}, raw).full_url, raw)
        for url in ('http://github.com/file', 'http://raw.githubusercontent.com/file',
                    'https://example.org/file', 'https://user:password@github.com/file',
                    'https://raw.githubusercontent.com.example.org/file'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                redirect.redirect_request(request, None, 302, '', {}, url)

    def test_duplicate_manifest_keys_rejected(self):
        with self.assertRaises(ValueError): bootstrap.parse(b'{"readiness":"prototype","readiness":"release"}')

    def test_retired_release_assembler_is_never_installable(self):
        files = legacy_contents(); files.update({'patch-steam-link.cmd': b'fixture cmd', 'patch-steam-link.sh': b'fixture bash'})
        with self.assertRaises(ValueError): self.extract(packaging.assemble(files))
        self.assertEqual(list((self.path/'package').iterdir()), [])

    def test_portable_python_pin_license_and_no_automatic_download(self):
        def runtime(files):
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, 'w') as target:
                for name, raw in files.items(): target.writestr(name, raw)
            return stream.getvalue()
        files = {'python.exe': b'MZ_NON_EXECUTABLE_TEST_FIXTURE', 'LICENSE.txt': b'fixture license', 'python314.zip': b'fixture stdlib', 'python314._pth': b'python314.zip\n.\n# import site\n'}
        raw = runtime(files)
        digest = hashlib.sha256(raw).hexdigest()
        url = 'https://www.python.org/ftp/python/3.14.0/python-3.14.0-embed-amd64.zip'
        output=packaging.windows_python(raw,digest,url)
        self.assertIn('setup/runtime/python/LICENSE.txt',output)
        self.assertEqual(output['setup/runtime/python/python314._pth'],b'python314.zip\n.\n# import site\n../vendor\n')
        with self.assertRaises(ValueError): packaging.windows_python(raw, '0' * 64, url)
        with self.assertRaises(ValueError): packaging.windows_python(raw, digest, 'https://example.org/runtime.zip')
        del files['LICENSE.txt']; missing = runtime(files)
        with self.assertRaises(ValueError): packaging.windows_python(missing, hashlib.sha256(missing).hexdigest(), url)

    def piped_bootstrap(self, inspection, maintenance=False, bridge_preview=False, corrupt_pin=None):
        import pty
        import signal
        report = self.path / 'terminal-result.json'
        requests = self.path / 'requests.json'
        files = manager_contents() if maintenance else inspection_contents() if inspection else contents()
        files['setup/steam_link_setup.py'] = (
            'import sys,json,pathlib\n'
            f'pathlib.Path({str(report)!r}).write_text(json.dumps({{"tty":sys.stdin.isatty(),"args":sys.argv[1:]}}))\n'
            'assert sys.stdin.isatty()\n').encode()
        raw = (repack(bridge_archive(), {'setup/steam_link_setup.py': files['setup/steam_link_setup.py']}) if not inspection and not maintenance else
               archive(files, inspection=inspection, maintenance=maintenance))
        release = self.release(inspection, maintenance, bridge_preview)
        release['assets'][0].update(size=len(raw), digest='sha256:' + hashlib.sha256(raw).hexdigest())
        endpoint = '/releases/tags/' + bootstrap.BRIDGE_TAG if bridge_preview else '/releases/tags/' + bootstrap.MANAGER_TAG if maintenance else '/releases/tags/' + bootstrap.INSPECTION_TAG if inspection else '/releases/tags/' + bootstrap.BRIDGE_TAG
        api_url = 'https://api.github.com/repos/' + bootstrap.REPO + endpoint
        asset_url = release['assets'][0]['browser_download_url']
        # Only network I/O and the fixture archive's exact pins are replaced.
        # The default must request the immutable repository URL without asking
        # GitHub Releases (which can be empty). Keep real terminal/child behavior.
        direct = not inspection and not maintenance
        if direct:
            asset_url = bootstrap.repository_asset()['browser_download_url']
            size = len(raw) + (1 if corrupt_pin == 'size' else 0)
            digest = '0' * 64 if corrupt_pin == 'digest' else hashlib.sha256(raw).hexdigest()
            injected = f'REPOSITORY_BYTES = {size!r}\nREPOSITORY_SHA256 = {digest!r}\n'
        else:
            injected = ''
        injected += ('fixture_requests = []\n'
                     'def fixture_download(url, limit):\n'
                     '    fixture_requests.append([url, limit])\n'
                     f'    Path({str(requests)!r}).write_text(json.dumps(fixture_requests))\n')
        if not direct:
            injected += f'    if url == {api_url!r}: return {json.dumps(release).encode()!r}\n'
        injected += (f'    if url == {asset_url!r}:\n'
                     f'        require(len({raw!r}) <= limit, "Download exceeds size bound.")\n'
                     f'        return {raw!r}\n'
                     '    raise ValueError("Unexpected fixture endpoint")\n'
                     'download = fixture_download\n')
        script = (ROOT / 'get.sh').read_text().replace("if __name__ == '__main__':", injected + "if __name__ == '__main__':")
        master, slave = pty.openpty()
        def controlling_terminal():
            import fcntl
            import termios
            os.setsid(); fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        child = None
        try:
            with (self.path / 'terminal.log').open('wb') as output:
                child = subprocess.Popen(['bash', '-s', '--'] + (['--bridge-preview'] if bridge_preview else ['--manage'] if maintenance else ['--inspect'] if inspection else []), stdin=subprocess.PIPE,
                                         stdout=output, stderr=subprocess.STDOUT, preexec_fn=controlling_terminal)
                try:
                    child.communicate(script.encode(), timeout=15)
                except BaseException:
                    os.killpg(child.pid, signal.SIGKILL); child.wait(); raise
            requested = json.loads(requests.read_text())
            if direct:
                self.assertEqual(requested, [[asset_url, size]], 'Default requests only the immutable repository archive')
            if corrupt_pin:
                self.assertEqual(child.returncode, 2, (self.path / 'terminal.log').read_text())
                self.assertFalse(report.exists(), 'Unverified archive must never start the wizard')
                self.assertIn('SHA-256 check', (self.path / 'terminal.log').read_text())
                return
            self.assertEqual(child.returncode, 0, (self.path / 'terminal.log').read_text())
            data = json.loads(report.read_text())
            self.assertTrue(data['tty']); self.assertEqual(data['args'][0], '--bundle')
            self.assertEqual(data['args'][2:], ['--manage'] if maintenance else ['--inspect'] if inspection else [])
            self.assertFalse(Path(data['args'][1]).exists(), 'Own download/runtime removed after child exits')
        finally:
            os.close(master); os.close(slave)

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_piped_bootstrap_gives_wizard_actual_terminal(self):
        self.piped_bootstrap(False)

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_repository_bootstrap_refuses_wrong_size_or_digest_before_wizard(self):
        for corrupt in ('size', 'digest'):
            with self.subTest(pin=corrupt):
                self.piped_bootstrap(False, corrupt_pin=corrupt)

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_piped_inspection_preserves_terminal_and_explicit_mode(self):
        self.piped_bootstrap(True)

    def test_manager_only_selects_its_own_pinned_release(self):
        self.assertEqual(bootstrap.select_asset(self.release(maintenance=True), maintenance=True)['name'], bootstrap.MANAGER_ASSET)
        for release, inspection, maintenance in ((self.release(True),False,True), (self.release(maintenance=True),True,False),
                                                 (self.release(),False,True), (self.release(maintenance=True),False,False)):
            with self.subTest(release=release['tag_name']), self.assertRaises(ValueError):
                bootstrap.select_asset(release,inspection,maintenance)

    def test_manager_archive_preserves_mode_separation(self):
        target = self.extract(archive(maintenance=True), maintenance=True)
        self.assertTrue((target / 'setup/frame_native_uninstall.py').is_file())
        self.assertFalse((target / 'steam-link-bundle.json').exists())
        for inspection in (False,True):
            with tempfile.TemporaryDirectory(dir=self.path) as temporary:
                root=Path(temporary); zipped=root/'a.zip'; zipped.write_bytes(archive(maintenance=True)); out=root/'out';out.mkdir()
                with self.assertRaises(ValueError): bootstrap.extract(zipped,out,inspection)
                self.assertEqual(list(out.iterdir()),[])

    def test_manager_rejects_install_payload_before_extraction(self):
        for name in ('steam-link-bundle.json','pc-provider/driver.dll','receiver/lib.so','setup/unknown.py'):
            with self.subTest(name=name), tempfile.TemporaryDirectory(dir=self.path) as temporary:
                files=manager_contents();files[name]=b'NOT EXECUTED'
                root=Path(temporary);zipped=root/'a.zip';zipped.write_bytes(archive(files,maintenance=True));out=root/'out';out.mkdir()
                with self.assertRaises(ValueError): bootstrap.extract(zipped,out,maintenance=True)
                self.assertEqual(list(out.iterdir()),[])

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_manager_uses_real_terminal_and_never_install_argument(self):
        self.piped_bootstrap(False, maintenance=True)

    def test_bridge_preview_exact_pinned_release(self):
        release = self.release(bridge_preview=True)
        self.assertEqual(bootstrap.select_asset(release, bridge_preview=True)['name'], bootstrap.BRIDGE_ASSET)
        for field, value in [('draft', True), ('prerelease', False), ('tag_name', 'another-bridge')]:
            candidate = self.release(bridge_preview=True); candidate[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                bootstrap.select_asset(candidate, bridge_preview=True)
        for flags in ((True, False, False), (False, True, False),
                      (True, False, True), (False, True, True)):
            with self.subTest(flags=flags), self.assertRaises(ValueError): bootstrap.select_asset(release, *flags)
        for candidate in (self.release(True), self.release(maintenance=True)):
            with self.subTest(candidate=candidate['tag_name']), self.assertRaises(ValueError):
                bootstrap.select_asset(candidate, bridge_preview=True)

    def test_actual_bridge_assembler_to_validated_extract(self):
        raw = bridge_archive()
        target = self.extract(raw, bridge_preview=True)
        with zipfile.ZipFile(io.BytesIO(raw)) as package:
            for name in package.namelist():
                if name != 'bootstrap-package.json': self.assertEqual((target / name).read_bytes(), package.read(name))
        bundle = json.loads((target / 'stream-bridge-bundle.json').read_bytes())
        self.assertEqual(bundle['schema'], 'sfnp-stream-bridge-bundle-v1')
        self.assertEqual(bundle['readiness'], 'release')
        self.assertTrue((target / 'setup/stream_bridge_install.py').is_file())
        self.assertTrue((target / 'setup/runtime/openssh/ssh.exe').is_file())
        self.assertTrue((target / 'setup/steam-link-askpass.exe').is_file())
        if os.name != 'nt':self.assertTrue(os.access(target / 'setup/steam-link-askpass.sh', os.X_OK))
        self.assertFalse((target / 'steam-link-bundle.json').exists())

    def test_bridge_modes_cannot_be_interchanged(self):
        raw = bridge_archive()
        cases = [(raw, flags) for flags in ((True,False,False),(False,True,False),
                                           (True,False,True),(False,True,True))]
        cases += [(archive(inspection=i, maintenance=m, legacy=not i and not m), (False,False,True))
                  for i,m in ((False,False),(True,False),(False,True))]
        for index, (value, flags) in enumerate(cases):
            with self.subTest(index=index), tempfile.TemporaryDirectory(dir=self.path) as temporary:
                base = Path(temporary); zipped = base / 'a.zip'; zipped.write_bytes(value)
                target = base / 'target'; target.mkdir()
                with self.assertRaises(ValueError): bootstrap.extract(zipped, target, *flags)
                self.assertEqual(list(target.iterdir()), [])

    def test_actual_bridge_prototype_refused_before_extraction(self):
        with self.assertRaises(ValueError): self.extract(bridge_archive(release=False), bridge_preview=True)
        self.assertEqual(list((self.path / 'package').iterdir()), [])

    def test_bridge_inner_readiness_and_schema_refused_before_extraction(self):
        raw = bridge_archive()
        for field, value in [('readiness','prototype'), ('schema','sfnp-steam-link-setup-v1')]:
            with self.subTest(field=field), tempfile.TemporaryDirectory(dir=self.path) as temporary:
                with zipfile.ZipFile(io.BytesIO(raw)) as package: bundle = json.loads(package.read('stream-bridge-bundle.json'))
                bundle[field] = value
                candidate = repack(raw, {'stream-bridge-bundle.json':json.dumps(bundle).encode()})
                base = Path(temporary); zipped = base / 'a.zip'; zipped.write_bytes(candidate)
                target = base / 'target'; target.mkdir()
                with self.assertRaises(ValueError): bootstrap.extract(zipped, target, bridge_preview=True)
                self.assertEqual(list(target.iterdir()), [])

    def test_bridge_preview_keeps_hash_and_mixed_package_refusals(self):
        raw = bridge_archive()
        legacy = legacy_contents()['steam-link-bundle.json']
        cases = [(repack(raw, {'steam-link-bundle.json':legacy}), (False,False,True))]
        files = legacy_contents(); files['stream-bridge-bundle.json'] = b'{"schema":"sfnp-stream-bridge-bundle-v1","readiness":"release"}'
        cases.append((archive(files,legacy=True), (False,False,False)))
        # Same length but wrong bytes, with the original reviewed file pin.
        with zipfile.ZipFile(io.BytesIO(raw)) as package:
            values = {name:package.read(name) for name in package.namelist()}
        name = 'setup/stream_bridge_install.py'; values[name] = b'X' + values[name][1:]
        out = io.BytesIO()
        with zipfile.ZipFile(out,'w') as package:
            for name,value in values.items(): package.writestr(name,value)
        cases.append((out.getvalue(), (False,False,True)))
        for index,(value,flags) in enumerate(cases):
            with self.subTest(index=index), tempfile.TemporaryDirectory(dir=self.path) as temporary:
                base=Path(temporary); zipped=base/'a.zip'; zipped.write_bytes(value); target=base/'target'; target.mkdir()
                with self.assertRaises(ValueError): bootstrap.extract(zipped,target,*flags)

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_bridge_preview_uses_actual_terminal_and_bundle_selected_engine(self):
        self.piped_bootstrap(False, bridge_preview=True)

    def test_actual_powershell_archive_functions(self):
        if not POWERSHELL: self.skipTest('Pass --pwsh to run actual PowerShell parser/extraction functions')
        valid = self.path / 'valid.zip'; valid.write_bytes(archive())
        legacy = self.path / 'legacy.zip'; legacy.write_bytes(archive(legacy=True))
        bad = self.path / 'bad.zip'; bad.write_bytes(archive(mutate=lambda v: v.update(readiness='prototype')))
        inspection = self.path / 'inspection.zip'
        inspection_files=inspection_contents()
        inspection_files['setup/steam_link_setup_textual.py']=b'# Owned Textual fixture\n'
        inspection_files['setup/steam_link_setup_vendor.py']=b'# Owned loader fixture\n'
        inspection_files.update({'setup/runtime/vendor/textual/fixture_'+str(i)+'_'+'x'*100+'.py':b'' for i in range(1400)})
        inspection.write_bytes(archive(inspection_files,inspection=True))
        files = inspection_contents(); files['pc-provider/driver.dll'] = b'UNEXECUTED NATIVE FIXTURE'
        payload = self.path / 'payload.zip'; payload.write_bytes(archive(files, inspection=True))
        manager = self.path / 'manager.zip'; manager.write_bytes(archive(maintenance=True))
        files = manager_contents(); files['pc-provider/driver.dll'] = b'UNEXECUTED NATIVE FIXTURE'
        manager_payload = self.path / 'manager-payload.zip'; manager_payload.write_bytes(archive(files, maintenance=True))
        bridge = self.path / 'bridge.zip'; bridge.write_bytes(bridge_archive())
        bridge_prototype = self.path / 'bridge-prototype.zip'; bridge_prototype.write_bytes(bridge_archive(release=False))
        with zipfile.ZipFile(bridge) as package: inner = json.loads(package.read('stream-bridge-bundle.json'))
        inner['readiness'] = 'prototype'
        bridge_inner = self.path / 'bridge-inner-prototype.zip'
        bridge_inner.write_bytes(repack(bridge.read_bytes(), {'stream-bridge-bundle.json':json.dumps(inner).encode()}))
        report = self.path / 'ps-result.json'
        script = self.path / 'check.ps1'
        script.write_text(r'''param($Source, $Valid, $Bad, $Output, $Report, $Inspection, $Payload, $Manager, $ManagerPayload, $Bridge, $BridgePrototype, $BridgeInner, $Legacy)
$ErrorActionPreference='Stop'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($Source,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Bootstrap did not parse.' }
$functions=$ast.FindAll({ param($item) $item -is [Management.Automation.Language.FunctionDefinitionAst] },$true)
foreach ($fn in $functions) { Invoke-Expression $fn.Extent.Text }
$pinNames=@('sfnpRepo','sfnpRepositoryCommit','sfnpRepositoryAsset','sfnpRepositoryBytes','sfnpRepositorySha256')
$pins=$ast.FindAll({ param($item) $item -is [Management.Automation.Language.AssignmentStatementAst] -and $item.Left -is [Management.Automation.Language.VariableExpressionAst] -and $item.Left.VariablePath.UserPath -in $pinNames },$true)
if ($pins.Count -ne $pinNames.Count) { throw 'Repository pins are ambiguous.' }
foreach ($pin in $pins) { Invoke-Expression $pin.Extent.Text }
$repository=Get-SfnpRepositoryAsset
if ($repository.browser_download_url -cne ('https://raw.githubusercontent.com/'+$sfnpRepo+'/'+$sfnpRepositoryCommit+'/'+$sfnpRepositoryAsset) -or
    $repository.size -ne 18203820 -or $repository.digest -cne 'sha256:6d8f6ec93cc80a2220892184966d4b9b4568ed86a9e51e09382570e53da6b2df') { throw 'Repository URL/size/hash pin differs.' }
$target=Join-Path $Output 'ps-valid'; [void][IO.Directory]::CreateDirectory($target)
$runtime=Expand-SfnpPackage $Valid $target
if ($runtime -ne 'setup/runtime/python/python.exe') { throw 'Wrong verified runtime.' }
$badTarget=Join-Path $Output 'ps-bad'; [void][IO.Directory]::CreateDirectory($badTarget)
$refused=$false
try { Expand-SfnpPackage $Bad $badTarget | Out-Null } catch { $refused=$true }
if (-not $refused -or @(Get-ChildItem $badTarget -Recurse).Count -ne 0) { throw 'Prototype extraction did not stop.' }
$inspectionTarget=Join-Path $Output 'ps-inspection'; [void][IO.Directory]::CreateDirectory($inspectionTarget)
$inspectionRuntime=Expand-SfnpPackage $Inspection $inspectionTarget $true
if ($inspectionRuntime -ne 'setup/runtime/python/python.exe' -or -not (Test-Path (Join-Path $inspectionTarget 'setup-inspection.json'))) { throw 'Inspection did not verify.' }
foreach ($case in @(@($Inspection,$false),@($Valid,$true),@($Payload,$true))) {
 $caseTarget=Join-Path $Output ([Guid]::NewGuid().ToString('N'));[void][IO.Directory]::CreateDirectory($caseTarget)
 $blocked=$false;try { Expand-SfnpPackage $case[0] $caseTarget $case[1] | Out-Null } catch { $blocked=$true }
 if (-not $blocked -or @(Get-ChildItem $caseTarget -Recurse).Count -ne 0) { throw 'Inspection/release or payload gate failed.' }
}
$managerTarget=Join-Path $Output 'ps-manager';[void][IO.Directory]::CreateDirectory($managerTarget)
$managerRuntime=Expand-SfnpPackage $Manager $managerTarget $false $true
if ($managerRuntime -ne 'setup/runtime/python/python.exe' -or -not (Test-Path (Join-Path $managerTarget 'setup/frame_native_uninstall.py'))) { throw 'Manager did not verify.' }
foreach ($case in @(@($Manager,$false,$false),@($Manager,$true,$false),@($Valid,$false,$true),@($Inspection,$false,$true),@($ManagerPayload,$false,$true))) {
 $caseTarget=Join-Path $Output ([Guid]::NewGuid().ToString('N'));[void][IO.Directory]::CreateDirectory($caseTarget)
 $blocked=$false;try { Expand-SfnpPackage $case[0] $caseTarget $case[1] $case[2] | Out-Null } catch { $blocked=$true }
 if (-not $blocked -or @(Get-ChildItem $caseTarget -Recurse).Count -ne 0) { throw 'Manager mode/payload gate failed.' }
}
$bridgeTarget=Join-Path $Output 'ps-bridge';[void][IO.Directory]::CreateDirectory($bridgeTarget)
$bridgeRuntime=Expand-SfnpPackage $Bridge $bridgeTarget $false $false $true
if ($bridgeRuntime -ne 'setup/runtime/python/python.exe' -or -not (Test-Path (Join-Path $bridgeTarget 'setup/stream_bridge_install.py'))) { throw 'Bridge did not verify.' }
foreach ($case in @(@($Legacy,$false,$false,$false),@($Bridge,$true,$false,$false),@($Bridge,$false,$true,$false),
                    @($Bridge,$true,$false,$true),@($Bridge,$false,$true,$true),
                    @($Legacy,$false,$false,$true),@($Inspection,$false,$false,$true),@($Manager,$false,$false,$true),
                    @($BridgePrototype,$false,$false,$true),@($BridgeInner,$false,$false,$true))) {
 $caseTarget=Join-Path $Output ([Guid]::NewGuid().ToString('N'));[void][IO.Directory]::CreateDirectory($caseTarget)
 $blocked=$false;try { Expand-SfnpPackage $case[0] $caseTarget $case[1] $case[2] $case[3] | Out-Null } catch { $blocked=$true }
 if (-not $blocked -or @(Get-ChildItem $caseTarget -Recurse).Count -ne 0) { throw 'Bridge mode/readiness gate failed.' }
}
$unsafe=@('../escape' ,'/escape','C:/escape','x\escape','x//escape','x/CON.txt','x/end.','x/end ','x/CONIN$','x/CONOUT$',('x/COM'+[char]0xB9),('x/LPT'+[char]0xB2),'x/a|b','x/a?b')
foreach ($name in $unsafe) {
 $blocked=$false; try { Get-SfnpName $name | Out-Null } catch { $blocked=$true }
 if (-not $blocked) { throw ('Unsafe path accepted: '+$name) }
}
@{parsed=$true;exact_runtime=$runtime;prototype_refused=$true;unsafe_paths=$unsafe.Count;inspection_verified=$true;inspection_native_payload_refused=$true;bridge_verified=$true;bridge_prototypes_refused=$true;bridge_scope_separated=$true;scope='ActualPowerShellFunctions_NoNetwork_NoWindowsACL_NoDevice'} | ConvertTo-Json | Set-Content $Report
''')
        result = subprocess.run([POWERSHELL, '-NoLogo', '-NoProfile', '-NonInteractive', '-File', str(script),
                                 str(ROOT / 'get.ps1'), str(valid), str(bad), str(self.path), str(report), str(inspection), str(payload), str(manager), str(manager_payload), str(bridge), str(bridge_prototype), str(bridge_inner), str(legacy)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False)
        self.assertEqual(result.returncode, 0, result.stdout.decode(errors='replace'))
        data = json.loads(report.read_text(encoding='utf-8-sig'))
        self.assertTrue(data['prototype_refused']); self.assertTrue(data['inspection_verified']); self.assertTrue(data['inspection_native_payload_refused'])
        self.assertTrue(data['bridge_verified']); self.assertTrue(data['bridge_prototypes_refused']); self.assertTrue(data['bridge_scope_separated'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pwsh')
    parser.add_argument('--output', type=Path)
    args, test_args = parser.parse_known_args()
    POWERSHELL, OUT = args.pwsh, args.output
    if OUT: OUT.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *test_args])
