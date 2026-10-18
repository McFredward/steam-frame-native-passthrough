#!/usr/bin/env python3
"""Run the actual online bootstrap parsers against local release/ZIP fixtures."""
from __future__ import annotations
import argparse
import hashlib
import io
import importlib.util
import json
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


def archive(files=None, *, mutate=None, extra=None):
    files = contents() if files is None else files
    manifest = {'schema': 'sfnp-bootstrap-package-v1', 'readiness': 'release',
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

    def release(self):
        return {'draft': False, 'prerelease': False, 'tag_name': 'v1.0.0',
                'assets': [{'name': bootstrap.ASSET, 'state': 'uploaded', 'size': 512,
                            'digest': 'sha256:' + 'a' * 64,
                            'browser_download_url': 'https://github.com/' + bootstrap.REPO + '/releases/download/v1.0.0/' + bootstrap.ASSET}]}

    def extract(self, raw):
        target = self.path / 'package'; target.mkdir()
        zipped = self.path / 'download.zip'; zipped.write_bytes(raw)
        bootstrap.extract(zipped, target)
        return target

    def test_exact_public_asset(self):
        self.assertEqual(bootstrap.select_asset(self.release())['size'], 512)

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

    def test_actual_powershell_archive_functions(self):
        if not POWERSHELL: self.skipTest('Pass --pwsh to run actual PowerShell parser/extraction functions')
        valid = self.path / 'valid.zip'; valid.write_bytes(archive())
        bad = self.path / 'bad.zip'; bad.write_bytes(archive(mutate=lambda v: v.update(readiness='prototype')))
        report = self.path / 'ps-result.json'
        script = self.path / 'check.ps1'
        script.write_text(r'''param($Source, $Valid, $Bad, $Output, $Report)
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
$unsafe=@('../escape','/escape','C:/escape','x\escape','x//escape','x/CON.txt','x/end.','x/end ','x/CONIN$','x/CONOUT$',('x/COM'+[char]0xB9),('x/LPT'+[char]0xB2),'x/a|b','x/a?b')
foreach ($name in $unsafe) {
 $blocked=$false; try { Get-SfnpName $name | Out-Null } catch { $blocked=$true }
 if (-not $blocked) { throw ('Unsafe path accepted: '+$name) }
}
@{parsed=$true;exact_runtime=$runtime;prototype_refused=$true;unsafe_paths=$unsafe.Count;scope='ActualPowerShellFunctions_NoNetwork_NoWindowsACL_NoDevice'} | ConvertTo-Json | Set-Content $Report
''')
        result = subprocess.run([POWERSHELL, '-NoLogo', '-NoProfile', '-NonInteractive', '-File', str(script),
                                 str(ROOT / 'get.ps1'), str(valid), str(bad), str(self.path), str(report)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False)
        self.assertEqual(result.returncode, 0, result.stdout.decode(errors='replace'))
        data = json.loads(report.read_text(encoding='utf-8-sig'))
        self.assertTrue(data['prototype_refused'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pwsh')
    parser.add_argument('--output', type=Path)
    args, test_args = parser.parse_known_args()
    POWERSHELL, OUT = args.pwsh, args.output
    if OUT: OUT.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *test_args])
