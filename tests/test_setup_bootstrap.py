#!/usr/bin/env python3
"""Run the actual online bootstrap parsers against local release/ZIP fixtures."""
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
POWERSHELL = None
OUT = None


def contents():
    return {'steam-link-bundle.json': json.dumps({'schema': 'sfnp-steam-link-setup-v1', 'readiness': 'release'}).encode(),
            'setup/steam_link_setup.py': b'# Non-executable fixture content.\n',
            'setup/runtime/python/python.exe': b'FIXTURE ONLY: NOT A REAL EXECUTABLE\n'}


def inspection_contents():
    return {'setup-inspection.json': json.dumps({'schema': 'sfnp-setup-inspection-v1',
            'expected_pc': {}, 'runtime_files': {}}).encode(),
            'setup/steam_link_setup.py': b'# Non-executable inspection fixture.\n',
            'setup/runtime/python/python.exe': b'FIXTURE ONLY: NOT A REAL EXECUTABLE\n'}


def archive(files=None, *, mutate=None, extra=None, inspection=False):
    files = (inspection_contents() if inspection else contents()) if files is None else files
    manifest = {'schema': 'sfnp-bootstrap-inspection-v1' if inspection else 'sfnp-bootstrap-package-v1',
                'readiness': 'inspection-only' if inspection else 'release',
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


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=OUT)
        self.path = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def release(self, inspection=False):
        tag = bootstrap.INSPECTION_TAG if inspection else 'v1.0.0'
        asset_name = bootstrap.INSPECTION_ASSET if inspection else bootstrap.ASSET
        return {'draft': False, 'prerelease': inspection, 'tag_name': tag,
                'assets': [{'name': asset_name, 'state': 'uploaded', 'size': 512,
                            'digest': 'sha256:' + 'a' * 64,
                            'browser_download_url': 'https://github.com/' + bootstrap.REPO + '/releases/download/' + tag + '/' + asset_name}]}

    def extract(self, raw, inspection=False):
        target = self.path / 'package'; target.mkdir()
        zipped = self.path / 'download.zip'; zipped.write_bytes(raw)
        bootstrap.extract(zipped, target, inspection)
        return target

    def test_exact_public_asset(self):
        self.assertEqual(bootstrap.select_asset(self.release())['size'], 512)

    def test_preview_wrappers_pin_reviewed_core_and_force_inspection(self):
        for name, source, flag in [('get-preview.ps1', 'get.ps1', '-InspectionOnly'),
                                   ('get-preview.sh', 'get.sh', '--inspect')]:
            text = (ROOT / name).read_text()
            digest = hashlib.sha256((ROOT / source).read_bytes()).hexdigest()
            self.assertIn(digest, text)
            self.assertIn('/2c2fdeeef5ce623c55b2c8489872531f8226a238/' + source, text)
            self.assertIn(flag, text)

    def test_explicit_inspection_release_is_separate_from_stable(self):
        release = self.release(True)
        self.assertEqual(bootstrap.select_asset(release, True)['name'], bootstrap.INSPECTION_ASSET)
        for candidate, mode in [(release, False), (self.release(), True)]:
            with self.assertRaises(ValueError): bootstrap.select_asset(candidate, mode)
        for field, value in [('draft', True), ('prerelease', False), ('tag_name', 'another-inspection')]:
            candidate = self.release(True); candidate[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): bootstrap.select_asset(candidate, True)
        candidate = self.release(True); candidate['assets'][0]['name'] = bootstrap.ASSET
        with self.assertRaises(ValueError): bootstrap.select_asset(candidate, True)

    def test_inspection_archive_has_no_installation_payload(self):
        files = inspection_contents(); files['LICENSE'] = b'MIT attribution fixture'
        target = self.extract(archive(files, inspection=True), True)
        self.assertEqual((target / 'LICENSE').read_bytes(), files['LICENSE'])
        self.assertEqual((target / 'setup-inspection.json').read_bytes(), inspection_contents()['setup-inspection.json'])
        self.assertFalse((target / 'steam-link-bundle.json').exists())

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
        self.assertFalse(bootstrap.arguments([])); self.assertTrue(bootstrap.arguments(['--inspect']))
        for args in (['--apply'], ['--inspect', '--apply'], ['--inspect', '--inspect'], ['--inspection']):
            with self.subTest(args=args), self.assertRaises(ValueError): bootstrap.arguments(args)

    def test_incomplete_or_unexpected_release_refused(self):
        for field, value in [('draft', True), ('prerelease', True), ('tag_name', '../main'), ('assets', [])]:
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
        files = contents(); files['steam-link-bundle.json'] = b'{"schema":"sfnp-steam-link-setup-v1","readiness":"prototype"}'
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
        for url in ('http://github.com/file', 'https://example.org/file', 'https://user:password@github.com/file'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                redirect.redirect_request(request, None, 302, '', {}, url)

    def test_duplicate_manifest_keys_rejected(self):
        with self.assertRaises(ValueError): bootstrap.parse(b'{"readiness":"prototype","readiness":"release"}')

    def test_actual_release_assembler_matches_bootstrap(self):
        files = contents(); files.update({'patch-steam-link.cmd': b'fixture cmd', 'patch-steam-link.sh': b'fixture bash'})
        target = self.extract(packaging.assemble(files))
        for name, raw in files.items(): self.assertEqual((target / name).read_bytes(), raw)

    def test_portable_python_pin_license_and_no_automatic_download(self):
        def runtime(files):
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, 'w') as target:
                for name, raw in files.items(): target.writestr(name, raw)
            return stream.getvalue()
        files = {'python.exe': b'MZ_NON_EXECUTABLE_TEST_FIXTURE', 'LICENSE.txt': b'fixture license', 'python314.zip': b'fixture stdlib'}
        raw = runtime(files)
        digest = hashlib.sha256(raw).hexdigest()
        url = 'https://www.python.org/ftp/python/3.14.0/python-3.14.0-embed-amd64.zip'
        self.assertIn('setup/runtime/python/LICENSE.txt', packaging.windows_python(raw, digest, url))
        with self.assertRaises(ValueError): packaging.windows_python(raw, '0' * 64, url)
        with self.assertRaises(ValueError): packaging.windows_python(raw, digest, 'https://example.org/runtime.zip')
        del files['LICENSE.txt']; missing = runtime(files)
        with self.assertRaises(ValueError): packaging.windows_python(missing, hashlib.sha256(missing).hexdigest(), url)

    def piped_bootstrap(self, inspection):
        import pty
        import signal
        report = self.path / 'terminal-result.json'
        files = inspection_contents() if inspection else contents()
        files['setup/steam_link_setup.py'] = (
            'import sys,json,pathlib\n'
            f'pathlib.Path({str(report)!r}).write_text(json.dumps({{"tty":sys.stdin.isatty(),"args":sys.argv[1:]}}))\n'
            'assert sys.stdin.isatty()\n').encode()
        raw = archive(files, inspection=inspection)
        release = self.release(inspection)
        release['assets'][0].update(size=len(raw), digest='sha256:' + hashlib.sha256(raw).hexdigest())
        endpoint = '/releases/tags/' + bootstrap.INSPECTION_TAG if inspection else '/releases/latest'
        api_url = 'https://api.github.com/repos/' + bootstrap.REPO + endpoint
        asset_url = release['assets'][0]['browser_download_url']
        # Only network I/O is replaced. Execute the actual Bash argv/heredoc,
        # Python core and a real child through curl|bash-shaped pipe input.
        injected = ('def fixture_download(url, limit):\n'
                    f'    if url == {api_url!r}: return {json.dumps(release).encode()!r}\n'
                    f'    if url == {asset_url!r}: return {raw!r}\n'
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
                child = subprocess.Popen(['bash', '-s', '--'] + (['--inspect'] if inspection else []), stdin=subprocess.PIPE,
                                         stdout=output, stderr=subprocess.STDOUT, preexec_fn=controlling_terminal)
                try:
                    child.communicate(script.encode(), timeout=15)
                except BaseException:
                    os.killpg(child.pid, signal.SIGKILL); child.wait(); raise
            self.assertEqual(child.returncode, 0, (self.path / 'terminal.log').read_text())
            data = json.loads(report.read_text())
            self.assertTrue(data['tty']); self.assertEqual(data['args'][0], '--bundle')
            self.assertEqual(data['args'][2:], ['--inspect'] if inspection else [])
            self.assertFalse(Path(data['args'][1]).exists(), 'Own download/runtime removed after child exits')
        finally:
            os.close(master); os.close(slave)

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_piped_bootstrap_gives_wizard_actual_terminal(self):
        self.piped_bootstrap(False)

    @unittest.skipUnless(os.name == 'posix', 'Actual controlling-terminal test uses POSIX PTY')
    def test_piped_inspection_preserves_terminal_and_explicit_mode(self):
        self.piped_bootstrap(True)

    def test_actual_powershell_archive_functions(self):
        if not POWERSHELL: self.skipTest('Pass --pwsh to run actual PowerShell parser/extraction functions')
        valid = self.path / 'valid.zip'; valid.write_bytes(archive())
        bad = self.path / 'bad.zip'; bad.write_bytes(archive(mutate=lambda v: v.update(readiness='prototype')))
        inspection = self.path / 'inspection.zip'; inspection.write_bytes(archive(inspection=True))
        files = inspection_contents(); files['pc-provider/driver.dll'] = b'UNEXECUTED NATIVE FIXTURE'
        payload = self.path / 'payload.zip'; payload.write_bytes(archive(files, inspection=True))
        report = self.path / 'ps-result.json'
        script = self.path / 'check.ps1'
        script.write_text(r'''param($Source, $Valid, $Bad, $Output, $Report, $Inspection, $Payload)
$ErrorActionPreference='Stop'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($Source,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Bootstrap did not parse.' }
$functions=$ast.FindAll({ param($item) $item -is [Management.Automation.Language.FunctionDefinitionAst] },$true)
foreach ($fn in $functions) { Invoke-Expression $fn.Extent.Text }
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
$unsafe=@('../escape' ,'/escape','C:/escape','x\escape','x//escape','x/CON.txt','x/end.','x/end ','x/CONIN$','x/CONOUT$',('x/COM'+[char]0xB9),('x/LPT'+[char]0xB2),'x/a|b','x/a?b')
foreach ($name in $unsafe) {
 $blocked=$false; try { Get-SfnpName $name | Out-Null } catch { $blocked=$true }
 if (-not $blocked) { throw ('Unsafe path accepted: '+$name) }
}
@{parsed=$true;exact_runtime=$runtime;prototype_refused=$true;unsafe_paths=$unsafe.Count;inspection_verified=$true;inspection_native_payload_refused=$true;scope='ActualPowerShellFunctions_NoNetwork_NoWindowsACL_NoDevice'} | ConvertTo-Json | Set-Content $Report
''')
        result = subprocess.run([POWERSHELL, '-NoLogo', '-NoProfile', '-NonInteractive', '-File', str(script),
                                 str(ROOT / 'get.ps1'), str(valid), str(bad), str(self.path), str(report), str(inspection), str(payload)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False)
        self.assertEqual(result.returncode, 0, result.stdout.decode(errors='replace'))
        data = json.loads(report.read_text(encoding='utf-8-sig'))
        self.assertTrue(data['prototype_refused']); self.assertTrue(data['inspection_verified']); self.assertTrue(data['inspection_native_payload_refused'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pwsh')
    parser.add_argument('--output', type=Path)
    args, test_args = parser.parse_known_args()
    POWERSHELL, OUT = args.pwsh, args.output
    if OUT: OUT.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *test_args])
