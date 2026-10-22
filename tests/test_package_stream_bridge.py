"""Real assembler with explicitly synthetic native/dependency artifacts; no install."""
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('sfnp_package_bridge',ROOT/'tools/package_stream_bridge.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
spec=importlib.util.spec_from_file_location('sfnp_fixture_textual_wheels',ROOT/'tests/test_textual_dependencies.py')
tw=importlib.util.module_from_spec(spec);spec.loader.exec_module(tw)
def encoded(value):return (json.dumps(value,sort_keys=True,indent=2)+'\n').encode()
def pe(dll=False):
    raw=bytearray(256);raw[:2]=b'MZ';struct.pack_into('<I',raw,60,64);raw[64:68]=b'PE\0\0';struct.pack_into('<H',raw,68,0x8664);struct.pack_into('<H',raw,86,0x2000 if dll else 0);struct.pack_into('<H',raw,88,0x20b);return bytes(raw)
def elf():
    raw=bytearray(80);raw[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',raw,16,3,183);return bytes(raw)
def helper_pe():
    # Explicitly synthetic x64 GUI image with bounded system-only imports.
    raw=bytearray(1024);raw[:256]=pe();struct.pack_into('<H',raw,70,1)
    struct.pack_into('<H',raw,84,240);struct.pack_into('<H',raw,156,2)
    struct.pack_into('<II',raw,208,0x1000,80)
    struct.pack_into('<IIII',raw,336,512,0x1000,512,512)
    for i,name in enumerate((b'kernel32.dll',b'shell32.dll',b'ws2_32.dll')):
        struct.pack_into('<IIIII',raw,512+i*20,1,0,0,0x1080+i*32,1)
        at=640+i*32;raw[at:at+len(name)+1]=name+b'\0'
    return bytes(raw)
def archive(files):
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        for name,raw in files.items():z.writestr(name,raw)
    return out.getvalue()

class Tests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='sfnp-package-test-');self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name);self.root=self.base/'repo';self.root.mkdir()
        for name in p.required_sources(ROOT):
            target=self.root/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((ROOT/name).read_bytes())
        self.pc=self.base/'provider.dll';self.pc.write_bytes(pe(True))
        self.layer=self.base/'layer.so';self.layer.write_bytes(elf())
        self.selector=self.base/'selector.so';self.selector.write_bytes(elf()+b'own-selector-fixture')
        self.askpass=self.base/'askpass.exe';self.askpass.write_bytes(helper_pe())
        self.python=self.base/'python.zip';self.python.write_bytes(archive({'python.exe':pe(),'LICENSE.txt':b'owned fixture license','python314.zip':b'owned fixture zip','python314._pth':b'python314.zip\n.\n'}))
        # No network use: fixture dependency pins are explicitly local and are
        # included in this fixture checkout's source-bound runtime profile.
        (self.root/'packaging/setup-python.json').write_bytes(encoded({'schema':'sfnp-portable-python-pin-v1','source_url':'https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip',**p.pin(self.python.read_bytes()),'release_notes':'https://www.python.org/downloads/release/python-3148/'}))
        sshfiles={'OpenSSH-Win64/ssh.exe':pe(),'OpenSSH-Win64/libcrypto.dll':pe(True),'OpenSSH-Win64/LICENSE.txt':b'owned SSH fixture license','OpenSSH-Win64/NOTICE.txt':b'owned SSH fixture notice'}
        self.ssh=self.base/'ssh.zip';self.ssh.write_bytes(archive(sshfiles))
        sshpin={'schema':'sfnp-portable-openssh-pin-v1','architecture':'x86_64','source_url':'https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64.zip','release_notes':'https://github.com/PowerShell/Win32-OpenSSH/releases/tag/10.0.0.0p2-Preview','upstream_status':'preview',**p.pin(self.ssh.read_bytes()),'files':{n:p.pin(raw) for n,raw in sshfiles.items()}}
        (self.root/'packaging/setup-openssh.json').write_bytes(encoded(sshpin))
        self.wheels=self.base/'textual-wheels'
        textualpin=json.loads((self.root/'packaging/setup-textual.json').read_bytes())
        tw.fixture_wheels(self.wheels,textualpin)
        (self.root/'packaging/setup-textual.json').write_bytes(encoded(textualpin))
        contract=p.load('sfnp_test_bridge_contract',self.root/'setup/stream_bridge_install.py')
        rp=lambda name:{'base':'steamvr','path':name,'bytes':1,'sha256':'a'*64}
        self.profile={'schema':p.PROFILE,'source_revision':'1'*40,'bridge_protocol':{'major':1,'minor':0,'wire_sha256':'a'*64},
                      'source_pins':{n:p.pin((self.root/n).read_bytes()) for n in p.required_sources(self.root)},
                      'artifacts':{'pc_provider':p.pin(self.pc.read_bytes()),'frame_layer':p.pin(self.layer.read_bytes()),'frame_selector':p.pin(self.selector.read_bytes())},
                      'pc':{'reviewed':True,'platform':'windows-x86_64','runtime_files':{'vrlink':rp('drivers/vrlink/bin/win64/driver_vrlink.dll'),'vrpathreg':rp('bin/win64/vrpathreg.exe')}},
                      'frame':{'reviewed':True,'generated':contract.GENERATED,'runtime_files':{'vrlink':rp('tools/vrlink/bin/linuxarm64/vrlink'),'compositor':rp('bin/linuxarm64/vrcompositor'),'fragment':rp('resources/shaders/vulkan/distort_ps_achromatic_se2.spv'),'openvr':rp('bin/linuxarm64/libopenvr_api.so')}}}
        self.path=self.base/'profile.json';self.save_profile()
    def save_profile(self):self.path.write_bytes(encoded(self.profile))
    def build(self,**kw):return p.build(self.pc,self.layer,self.selector,self.path,self.python,self.ssh,root=self.root,textual_wheels=self.wheels,askpass_exe=self.askpass,**kw)
    def validation(self,checks=None):
        value={'schema':p.VALIDATION,'result':'PASS','runtime_profile_sha256':p.pin(self.path.read_bytes())['sha256'],'source_revision':self.profile['source_revision'],'source_pins':self.profile['source_pins'],'artifacts':self.profile['artifacts'],'checks':{n:True for n in (checks or p.CHECKS)},'hardware_verified':False}
        path=self.base/'validation.json';path.write_bytes(encoded(value));return path

    def test_default_prototype_exact_payload_no_native_shader(self):
        raw,receipt=self.build();self.assertEqual(receipt['readiness'],'prototype');self.assertFalse(receipt['native_installable']);self.assertFalse(receipt['hardware_verified']);self.assertFalse(receipt['online_bootstrap_currently_accepts_schema'])
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            names=set(z.namelist());self.assertFalse(any(n.endswith('.spv') for n in names))
            self.assertIn('frame/tools/native_fragment_count1_interface.py',names)
            self.assertFalse(any('scene-contribution-atlas' in n or 'scene_presenter' in n or n.endswith('legacy.spv') for n in names))
            self.assertIn('setup/stream_bridge_install.py',names);self.assertIn('LICENSE',names)
            self.assertEqual(z.read('third_party/lz4/LICENSE'),(ROOT/'third_party/lz4/LICENSE').read_bytes())
            outer=json.loads(z.read('bootstrap-package.json'));self.assertEqual(outer['schema'],p.BOOTSTRAP);self.assertEqual(outer['readiness'],'prototype')
            self.assertEqual(set(outer['files']),names-{'bootstrap-package.json'})
            for n,pin in outer['files'].items():self.assertEqual(p.pin(z.read(n)),pin)
            bundle=json.loads(z.read('stream-bridge-bundle.json'));self.assertEqual(bundle['readiness'],'prototype');self.assertNotIn('steam-link-bundle.json',names)
            self.assertIn('setup/runtime/python/python.exe',names);self.assertIn('setup/runtime/openssh/ssh.exe',names)
            self.assertNotIn('setup/runtime/openssh/sshd.exe',names)
            self.assertIn('setup/steam_link_setup_textual.py',names)
            self.assertIn('setup/steam_link_setup_vendor.py',names)
            self.assertEqual(z.read('setup/steam-link-askpass.exe'),helper_pe())
            self.assertIn('setup/steam_link_setup_auth.py',names)
            self.assertIn('setup/steam_link_setup_askpass_client.py',names)
            self.assertEqual(z.getinfo('setup/steam-link-askpass.sh').external_attr>>16,0o100700)
            marker=json.loads(z.read('setup/runtime/vendor/SFNP-TEXTUAL-PROVENANCE.json'))
            self.assertEqual(marker['schema'],'sfnp-textual-vendor-v1')
            self.assertEqual(marker['packages']['textual']['version'],'8.2.8')
            self.assertIn(b'../vendor\n',z.read('setup/runtime/python/python314._pth'))

    def test_branch_logging_defaults_are_owned_data_without_native_changes(self):
        dev,_=self.build(channel='dev');main,_=self.build(channel='main')
        with zipfile.ZipFile(io.BytesIO(dev)) as a,zipfile.ZipFile(io.BytesIO(main)) as b:
            for name in ('pc-provider/logging-default.conf','frame/logging-default.conf'):
                self.assertEqual(a.read(name),b'Debug\n');self.assertEqual(b.read(name),b'Important\n')
            for name in a.namelist():
                if name.endswith(('.dll','.so','.exe','.py')):self.assertEqual(a.read(name),b.read(name),name)
            self.assertEqual(json.loads(a.read('stream-bridge-bundle.json'))['logging'],{'channel':'dev','level':'Debug'})
            self.assertEqual(json.loads(b.read('stream-bridge-bundle.json'))['logging'],{'channel':'main','level':'Important'})

    def test_reproducible_archive(self):
        first,r1=self.build();second,r2=self.build();self.assertEqual(first,second);self.assertEqual(r1,r2)

    def test_installer_and_patch_versions_are_source_pinned_and_stamped_independently(self):
        raw,receipt=self.build()
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            version=z.read('VERSION').decode().strip()
            components=json.loads(z.read('PATCH_VERSIONS.json'))
            versions={side:components[side] for side in ('pc','frame')}
            bundle=json.loads(z.read('stream-bridge-bundle.json'))
            self.assertEqual(version,(ROOT/'VERSION').read_text().strip());self.assertEqual(receipt['version'],version)
            self.assertEqual(bundle['version'],version)
            self.assertEqual(bundle['pc']['version'],components['pc']);self.assertEqual(bundle['frame']['version'],components['frame'])
            source=json.loads(z.read('stream-bridge-package-source.json'))
            self.assertEqual(source['version'],version);self.assertEqual(source['component_versions'],versions)
            self.assertEqual(receipt['component_versions'],versions)
            self.assertEqual(receipt['source_pins']['VERSION'],p.pin(z.read('VERSION')))
            self.assertEqual(receipt['source_pins']['PATCH_VERSIONS.json'],p.pin(z.read('PATCH_VERSIONS.json')))
            self.assertEqual(z.read('PATCH_VERSIONS.json'),(ROOT/'PATCH_VERSIONS.json').read_bytes())
        changed='0.0.1' if version=='0.0.0' else '0.0.0'
        (self.root/'VERSION').write_text(changed+'\n')
        with self.assertRaisesRegex(ValueError,'sources differ'):self.build()

    def test_installer_only_update_keeps_patch_versions_and_native_bytes(self):
        first,_=self.build()
        (self.root/'VERSION').write_text('9.8.7\n')
        self.profile['source_pins']['VERSION']=p.pin((self.root/'VERSION').read_bytes());self.save_profile()
        second,receipt=self.build()
        with zipfile.ZipFile(io.BytesIO(first)) as before,zipfile.ZipFile(io.BytesIO(second)) as after:
            previous=json.loads(before.read('stream-bridge-bundle.json'));current=json.loads(after.read('stream-bridge-bundle.json'))
            self.assertEqual(current['version'],'9.8.7');self.assertEqual(receipt['version'],'9.8.7')
            for side in ('pc','frame'):self.assertEqual(current[side]['version'],previous[side]['version'])
            self.assertEqual(current['files'],previous['files'])
            for name in current['files']:self.assertEqual(after.read(name),before.read(name))
            self.assertEqual(after.read('PATCH_VERSIONS.json'),before.read('PATCH_VERSIONS.json'))

    def test_pc_and_frame_patch_versions_may_differ(self):
        path=self.root/'PATCH_VERSIONS.json';value=json.loads(path.read_bytes())
        value.update(pc='1.4.0',frame='2.1.0-beta.2');path.write_bytes(encoded(value))
        self.profile['source_pins']['PATCH_VERSIONS.json']=p.pin(path.read_bytes());self.save_profile()
        raw,receipt=self.build()
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            bundle=json.loads(z.read('stream-bridge-bundle.json'))
            self.assertEqual(bundle['version'],(self.root/'VERSION').read_text().strip())
            self.assertEqual(bundle['pc']['version'],'1.4.0');self.assertEqual(bundle['frame']['version'],'2.1.0-beta.2')
            self.assertEqual(receipt['component_versions'],{'pc':'1.4.0','frame':'2.1.0-beta.2'})

    def test_patch_version_change_requires_reviewed_source_pin(self):
        path=self.root/'PATCH_VERSIONS.json';value=json.loads(path.read_bytes());value['pc']='0.2.12'
        path.write_bytes(encoded(value))
        with self.assertRaisesRegex(ValueError,'sources differ'):self.build()

    def test_patch_version_manifest_rejects_unknown_or_missing_semantic_versions(self):
        path=self.root/'PATCH_VERSIONS.json';original=json.loads(path.read_bytes())
        for invalid in (None,[],{**original,'schema':'unknown'},
                        {key:value for key,value in original.items() if key!='frame'},
                        {**original,'extra':True},{**original,'pc':None},{**original,'frame':12},
                        {**original,'pc':'1.02.3'},{**original,'frame':'1.2.3-01'}):
            with self.subTest(invalid=invalid):
                path.write_bytes(encoded(invalid));self.profile['source_pins']['PATCH_VERSIONS.json']=p.pin(path.read_bytes());self.save_profile()
                with self.assertRaises(ValueError):self.build()

    def test_missing_patch_version_manifest_cannot_create_package(self):
        (self.root/'PATCH_VERSIONS.json').unlink()
        with self.assertRaises(OSError):self.build()

    def test_release_requires_exact_complete_receipts(self):
        with self.assertRaisesRegex(ValueError,'receipts'):self.build(release=True)
        partial=self.validation({'pc_provider_build'})
        with self.assertRaisesRegex(ValueError,'missing'):self.build(release=True,validation_receipts=[partial])
        full=self.validation();raw,receipt=self.build(release=True,validation_receipts=[full])
        self.assertTrue(receipt['native_installable']);self.assertFalse(receipt['hardware_verified'])
        self.assertTrue(receipt['online_bootstrap_currently_accepts_schema'])
        with zipfile.ZipFile(io.BytesIO(raw)) as z:self.assertEqual(json.loads(z.read('stream-bridge-bundle.json'))['readiness'],'release')

    def test_mismatched_receipt_does_not_admit_release(self):
        receipt=self.validation();value=json.loads(receipt.read_bytes());value['artifacts']['frame_selector']['sha256']='0'*64;receipt.write_bytes(encoded(value))
        with self.assertRaisesRegex(ValueError,'different artifacts'):self.build(release=True,validation_receipts=[receipt])

    def test_source_change_and_missing_transitive_source_refused(self):
        source=self.root/'setup/stream_bridge_install.py';original=source.read_bytes();source.write_bytes(original+b'\n# fixture edit\n')
        with self.assertRaisesRegex(ValueError,'differ'):self.build()
        source.write_bytes(original);self.profile['source_pins'].pop('tools/native_fragment_count1_interface.py');self.save_profile()
        with self.assertRaisesRegex(ValueError,'transitive'):self.build()

    def test_tampered_native_and_wrong_arch_refused(self):
        self.selector.write_bytes(elf()+b'changed')
        with self.assertRaisesRegex(ValueError,'native artifact differs'):self.build()
        self.layer.write_bytes(b'MZ'+bytes(100));self.profile['artifacts']['frame_layer']=p.pin(self.layer.read_bytes());self.save_profile()
        with self.assertRaisesRegex(ValueError,'Frame shared library'):self.build()

    def test_actual_extracted_cli_check_is_prototype_read_only(self):
        raw,_=self.build();target=self.base/'extracted';target.mkdir()
        with zipfile.ZipFile(io.BytesIO(raw)) as z:z.extractall(target)
        home=self.base/'home';home.mkdir();before={str(n.relative_to(target)):n.read_bytes() for n in target.rglob('*') if n.is_file()}
        result=subprocess.run([sys.executable,'-I','-B',str(target/'setup/steam_link_setup.py'),'--bundle',str(target),'--check'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20,env=dict(os.environ,HOME=str(home),LOCALAPPDATA=str(home/'AppData/Local'),XDG_DATA_HOME=str(home/'.local/share'),XDG_CONFIG_HOME=str(home/'.config')))
        self.assertEqual(result.returncode,0,result.stderr.decode());value=json.loads(result.stdout);self.assertFalse(value['bridge_available']);self.assertIsNone(value['frame']);self.assertFalse(value['automatic_restart'])
        self.assertEqual(before,{str(n.relative_to(target)):n.read_bytes() for n in target.rglob('*') if n.is_file()});self.assertEqual(list(home.iterdir()),[])

    def test_publish_preserves_existing_archive(self):
        path=self.base/'existing.zip';path.write_bytes(b'user-owned')
        with self.assertRaises(FileExistsError):p.publish(path,b'new')
        self.assertEqual(path.read_bytes(),b'user-owned')

if __name__=='__main__':unittest.main()
