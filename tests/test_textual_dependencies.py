#!/usr/bin/env python3
"""Pinned offline Textual packaging, malicious wheel controls and real imports."""
from __future__ import annotations

import argparse
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_textual_dependencies', ROOT / 'tools/package_setup_textual.py')
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)


def archive(files):
    output = io.BytesIO()
    with warnings.catch_warnings(), zipfile.ZipFile(output, 'w', zipfile.ZIP_STORED) as wheel:
        warnings.simplefilter('ignore', UserWarning)
        for name, payload in files:
            wheel.writestr(name, payload)
    return output.getvalue()


def fixture_wheels(directory, pins):
    """Explicit synthetic fixture archives, never executable dependencies."""
    directory.mkdir()
    members = {}
    for name, item in pins['packages'].items():
        info = name.replace('-', '_') + '-' + item['version'] + '.dist-info/'
        contents = [(name.replace('-', '_') + '/__init__.py', b''),
                    (info + 'METADATA', ('Name: ' + name + '\nVersion: ' + item['version'] + '\n').encode()),
                    (info + 'WHEEL', b'Root-Is-Purelib: true\nTag: py3-none-any\n'),
                    *[(path, b'Complete synthetic original license\n') for path in item['licenses']]]
        members[name] = contents
        raw = archive(contents)
        item.update(d.pin(raw))
        (directory / item['filename']).write_bytes(raw)
    return members


class VendorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sfnp-textual-deps-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.cache = self.base / 'wheels'
        self.pins = json.loads((ROOT / 'packaging/setup-textual.json').read_bytes())
        self.members = fixture_wheels(self.cache, self.pins)

    def changed(self, name='textual', extra=(), replacement=None):
        raw = archive(self.members[name] + list(extra) if replacement is None else replacement)
        item = self.pins['packages'][name]
        item.update(d.pin(raw))
        (self.cache / item['filename']).write_bytes(raw)

    def test_complete_private_runtime_preserves_empty_modules_and_all_licenses(self):
        result = d.vendor(self.cache, self.pins)
        self.assertEqual(result[d.PREFIX + 'textual/__init__.py'], b'')
        provenance = json.loads(result[d.PREFIX + 'SFNP-TEXTUAL-PROVENANCE.json'])
        self.assertEqual(provenance['schema'], 'sfnp-textual-vendor-v1')
        self.assertEqual(set(provenance['packages']), d.PACKAGES)
        for name, item in self.pins['packages'].items():
            for path in item['licenses']:
                self.assertEqual(result[d.PREFIX + path], b'Complete synthetic original license\n')
            self.assertEqual(provenance['packages'][name]['sha256'], item['sha256'])

    def test_exact_dependency_set_and_official_pure_python_source_required(self):
        for field, value in [('filename', 'textual-8.2.8-cp313-win_amd64.whl'),
                             ('source_url', 'https://example.invalid/textual.whl'),
                             ('bytes', True), ('sha256', 'A' * 64), ('licenses', [])]:
            changed = copy.deepcopy(self.pins)
            changed['packages']['textual'][field] = value
            with self.subTest(field=field), self.assertRaises(d.VendorError):
                d.vendor(self.cache, changed)
        for name in d.PACKAGES:
            changed = copy.deepcopy(self.pins); del changed['packages'][name]
            with self.subTest(missing=name), self.assertRaises(d.VendorError):
                d.vendor(self.cache, changed)

    def test_tampered_wheel_does_not_execute_or_expand(self):
        item = self.pins['packages']['textual']; path = self.cache / item['filename']
        raw = path.read_bytes(); path.write_bytes(raw.replace(b'Complete synthetic', b'Changed! synthetic'))
        with self.assertRaisesRegex(d.VendorError, 'hash mismatch'):
            d.vendor(self.cache, self.pins)
        self.assertEqual(sorted(self.base.iterdir()), [self.cache])

    def test_purelib_metadata_and_package_identity_cannot_be_relabeled(self):
        original = list(self.members['textual'])
        for old, new in [(b'Name: textual', b'Name: imposter'), (b'Version: 8.2.8', b'Version: 8.2.7'),
                         (b'Root-Is-Purelib: true', b'Root-Is-Purelib: false'),
                         (b'Tag: py3-none-any', b'Tag: cp313-win_amd64')]:
            self.changed(replacement=[(name, raw.replace(old, new)) for name, raw in original])
            with self.subTest(changed=old), self.assertRaisesRegex(d.VendorError, 'identity or pure-Python'):
                d.vendor(self.cache, self.pins)

    def test_missing_license_is_rejected_after_valid_archive_hash(self):
        names = set(self.pins['packages']['textual']['licenses'])
        self.changed(replacement=[entry for entry in self.members['textual'] if entry[0] not in names])
        with self.assertRaisesRegex(d.VendorError, 'license is missing'):
            d.vendor(self.cache, self.pins)

    def test_traversal_aliases_hooks_and_case_collisions_are_rejected(self):
        bad_paths = ('../outside', '/outside', 'textual/../outside', 'textual\\escape', 'textual/name.',
                     'textual/name ', 'textual/NUL.txt', 'TEXTUAL/__init__.py', 'textual/hook.pth',
                     'textual/native.pyd', 'textual/data.so', 'textual-8.2.8.data/scripts/hook')
        for name in bad_paths:
            self.changed(extra=[(name, b'never runs')])
            with self.subTest(path=name), self.assertRaises(d.VendorError):
                d.vendor(self.cache, self.pins)

    def test_file_directory_conflicts_are_rejected(self):
        self.changed(extra=[('textual/collision', b'file'), ('textual/collision/child.py', b'')])
        with self.assertRaisesRegex(d.VendorError, 'file/directory collision'):
            d.vendor(self.cache, self.pins)

    @unittest.skipIf(os.name == 'nt', 'Windows link creation may require privileges; bootstrap tests cover Win32 links.')
    def test_linked_input_or_linked_wheel_member_refuses(self):
        item = self.pins['packages']['textual']; path = self.cache / item['filename']
        real = self.base / 'original.whl'; path.rename(real); path.symlink_to(real)
        with self.assertRaisesRegex(d.VendorError, 'linked'):
            d.vendor(self.cache, self.pins)
        path.unlink(); real.rename(path)
        link = zipfile.ZipInfo('textual/link'); link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.changed(extra=[(link, b'outside')])
        with self.assertRaisesRegex(d.VendorError, 'Linked wheel member'):
            d.vendor(self.cache, self.pins)

    def test_fetch_reuses_only_exact_inputs_without_network(self):
        from unittest.mock import patch
        with patch.object(d.urllib.request, 'build_opener', side_effect=AssertionError('network not allowed')):
            self.assertEqual(d.fetch(self.cache, self.pins), d.vendor(self.cache, self.pins))
            item = self.pins['packages']['textual']; (self.cache / item['filename']).write_bytes(b'bad')
            with self.assertRaises(d.VendorError):
                d.fetch(self.cache, self.pins)


def real_imports(cache, pilot=None):
    pins = json.loads((ROOT / 'packaging/setup-textual.json').read_bytes())
    files = d.vendor(cache, pins)
    with tempfile.TemporaryDirectory(prefix='sfnp-textual-private-import-') as temporary:
        base = Path(temporary)
        for name, raw in files.items():
            path = base / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(raw)
        vendor = base / d.PREFIX
        script = '''import sys, json, importlib.metadata
from pathlib import Path
root=Path(sys.argv[1]).resolve();sys.path.insert(0,str(root))
import textual,rich,pygments,markdown_it,mdit_py_plugins,platformdirs,typing_extensions,linkify_it,mdurl,uc_micro
from textual.app import App
from textual.widgets import Header,Footer,Button,Input,OptionList,RichLog,LoadingIndicator
modules=[textual,rich,pygments,markdown_it,mdit_py_plugins,platformdirs,typing_extensions,linkify_it,mdurl,uc_micro]
assert all(root in Path(m.__file__).resolve().parents for m in modules)
assert importlib.metadata.version('textual')=='8.2.8'
assert 'site-packages' not in '\\n'.join(sys.path[1:])
print(json.dumps({'private_modules':len(modules),'textual':importlib.metadata.version('textual'),'isolated':sys.flags.isolated,'runtime_pip':False}))
'''
        # -S suppresses this test runner's installed packages. Production uses
        # the embedded _pth or the explicit checked bundle loader, never global pip.
        result = subprocess.run([sys.executable, '-I', '-B', '-S', '-c', script, str(vendor)],
                                capture_output=True, text=True, timeout=30, check=False)
        if result.returncode:
            raise AssertionError(result.stderr)
        print(result.stdout.strip())
        if pilot is not None:
            runner="import sys,runpy;sys.path.insert(0,sys.argv[1]);path=sys.argv[2];sys.argv=[path];runpy.run_path(path,run_name='__main__')"
            result=subprocess.run([sys.executable,'-I','-B','-c',runner,str(vendor),str(pilot.resolve())],
                                  text=True,capture_output=True,timeout=120,check=False)
            print(result.stdout)
            if result.returncode:
                raise AssertionError(result.stderr)
            print(result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--wheels', type=Path)
    parser.add_argument('--pilot', type=Path)
    args, rest = parser.parse_known_args()
    if args.wheels:
        real_imports(args.wheels,args.pilot)
    unittest.main(argv=[sys.argv[0], *rest])
