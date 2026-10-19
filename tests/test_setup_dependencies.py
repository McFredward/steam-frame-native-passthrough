#!/usr/bin/env python3
"""Portable dependency archive validation; no download, installation or execution."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import struct
import sys
import unittest
import warnings
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_dependencies', ROOT/'tools/package_setup_dependencies.py')
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)


def pe(machine=0x8664):
    raw=bytearray(128);raw[:2]=b'MZ';struct.pack_into('<I',raw,60,64)
    raw[64:68]=b'PE\0\0';struct.pack_into('<H',raw,68,machine);struct.pack_into('<H',raw,88,0x20b)
    return bytes(raw)


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.pin=json.loads((ROOT/'packaging/setup-openssh.json').read_bytes())
        self.files={'OpenSSH-Win64/ssh.exe':pe(), 'OpenSSH-Win64/libcrypto.dll':pe(),
                    'OpenSSH-Win64/LICENSE.txt':b'FULL original fixture license\n',
                    'OpenSSH-Win64/NOTICE.txt':b'FULL original fixture notices\n',
                    'OpenSSH-Win64/install-sshd.ps1':b'ignored server must never ship'}
        self.reset_pins()
    def reset_pins(self):
        self.pin['files']={n:{'bytes':len(self.files[n]),'sha256':d.digest(self.files[n])}for n in d.OPENSSH_FILES if n in self.files}
    def archive(self, extra=()):
        output=io.BytesIO()
        with warnings.catch_warnings(),zipfile.ZipFile(output,'w',zipfile.ZIP_STORED)as archive:
            warnings.simplefilter('ignore',UserWarning)
            for name,payload in [*self.files.items(),*extra]:archive.writestr(name,payload)
        raw=output.getvalue();self.pin['bytes']=len(raw);self.pin['sha256']=d.digest(raw)
        return raw
    def test_only_client_dependency_full_licenses_and_source_are_published(self):
        raw=self.archive();result=d.windows_openssh(raw,self.pin)
        self.assertEqual(set(result),{'setup/runtime/openssh/'+name for name in
                                     ('ssh.exe','libcrypto.dll','LICENSE.txt','NOTICE.txt','source.json')})
        for name in d.OPENSSH_FILES:self.assertEqual(result['setup/runtime/openssh/'+Path(name).name],self.files[name])
        source=json.loads(result['setup/runtime/openssh/source.json'])
        self.assertEqual(source['sha256'],d.digest(raw));self.assertEqual(source['upstream_status'],'preview')
        self.assertEqual(source['files'],self.pin['files'])
    def test_outer_pin_rejects_same_length_ignored_server_mutation(self):
        raw=self.archive();old=b'ignored server must never ship';self.assertIn(old,raw)
        changed=raw.replace(old,b'altered server must never ship')
        self.assertEqual(len(changed),len(raw))
        with self.assertRaisesRegex(d.DependencyError,'archive pin mismatch'):d.windows_openssh(changed,self.pin)
    def test_unapproved_sources_architecture_and_false_stable_label_are_rejected(self):
        raw=self.archive()
        for key,value in [('source_url',self.pin['source_url'].replace('PowerShell','untrusted')),
                          ('release_notes','https://example.invalid/release'),('architecture','aarch64'),
                          ('upstream_status','stable')]:
            pin=copy.deepcopy(self.pin);pin[key]=value
            with self.subTest(field=key),self.assertRaises(d.DependencyError):d.windows_openssh(raw,pin)
    def test_missing_license_or_mismatched_selected_file_refuses(self):
        original=copy.deepcopy(self.pin)
        del self.files['OpenSSH-Win64/NOTICE.txt'];raw=self.archive()
        with self.assertRaisesRegex(d.DependencyError,'license file missing'):d.windows_openssh(raw,self.pin)
        self.files['OpenSSH-Win64/NOTICE.txt']=b'changed notices';raw=self.archive()
        with self.assertRaises(d.DependencyError):d.windows_openssh(raw,self.pin)
        self.pin=original;self.reset_pins();raw=self.archive();self.pin['files'].pop('OpenSSH-Win64/LICENSE.txt')
        with self.assertRaisesRegex(d.DependencyError,'license pins'):d.windows_openssh(raw,self.pin)
    def test_traversal_case_collision_redirected_and_encrypted_entries_refuse(self):
        for name in ('../outside','OpenSSH-Win64/../outside','OpenSSH-Win64\\ssh.exe','C:/bad','/absolute',
                     'OpenSSH-Win64/SSH.EXE','OpenSSH-Win64/name.','OpenSSH-Win64/name '):
            raw=self.archive([(name,b'bad')])
            with self.subTest(name=name),self.assertRaises(d.DependencyError):d.windows_openssh(raw,self.pin)
        link=zipfile.ZipInfo('OpenSSH-Win64/link');link.create_system=3;link.external_attr=(stat.S_IFLNK|0o777)<<16
        raw=self.archive([(link,b'outside')])
        with self.assertRaisesRegex(d.DependencyError,'Linked'):d.windows_openssh(raw,self.pin)
    def test_wrong_machine_and_size_bounds_refuse(self):
        self.files['OpenSSH-Win64/ssh.exe']=pe(0x14c);self.reset_pins();raw=self.archive()
        with self.assertRaisesRegex(d.DependencyError,'x64 PE'):d.windows_openssh(raw,self.pin)
        self.pin['files']['OpenSSH-Win64/ssh.exe']['bytes']=d.MAX_MEMBER+1
        with self.assertRaisesRegex(d.DependencyError,'Invalid dependency pin'):d.windows_openssh(raw,self.pin)
    def test_linux_metadata_is_official_baseline_and_never_a_sender_admission(self):
        original=json.loads((ROOT/'packaging/setup-linux-python.json').read_bytes())
        self.assertIs(d.validate_linux_python(original),original)
        self.assertEqual(set(original['architectures']),{'x86_64','aarch64'})
        for field,value in [('source_url','https://example.invalid/python.tar.gz'),('target','x86_64_v3-unknown-linux-gnu'),
                            ('glibc_min','0'),('executable','../../python')]:
            pin=copy.deepcopy(original);pin['architectures']['x86_64'][field]=value
            with self.subTest(field=field),self.assertRaises(d.DependencyError):d.validate_linux_python(pin)


if __name__=='__main__':unittest.main()
