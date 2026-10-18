#!/usr/bin/env python3
"""Actual wizard/remote-agent temp-filesystem tests; no SSH server or device."""
import builtins
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_guided_setup', ROOT/'setup/steam_link_setup.py')
s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)

VERIFIER = b'''import builtins,hashlib,json
def verify_package(root):
 value=json.loads((root/'package.json').read_bytes())
 for name,pin in value['files'].items():
  b=(root/name).read_bytes()
  if len(b)!=pin['bytes'] or hashlib.sha256(b).hexdigest()!=pin['sha256']:raise ValueError('pin')
 builtins.sfnp_fixture_verified=True
 return value
'''
NATIVE_SETUP = b'''import builtins,contextlib,hashlib,json,os,pathlib
class Setup:
 def __init__(self,home):self.home=home
 @contextlib.contextmanager
 def locked(self):yield
 def install(self,root):
  if not getattr(builtins,'sfnp_fixture_verified',False):raise ValueError('verifier must precede activation')
  data=self.home/'.local/share/SteamFrameNativePassthrough';files={}
  names=list(json.loads((root/'package.json').read_bytes())['files'])+['package.json']
  for name in names:
   b=(root/name).read_bytes();p=data/'package'/name;p.parent.mkdir(parents=True,exist_ok=True,mode=0o700);p.write_bytes(b);p.chmod(0o600);files[str(p)]=hashlib.sha256(b).hexdigest()
  helper=data/'tools/native_passthrough_setup.py';helper.parent.mkdir(parents=True,exist_ok=True,mode=0o700);helper.write_bytes((root/'tools/native_passthrough_setup.py').read_bytes());helper.chmod(0o600);files[str(helper)]=hashlib.sha256(helper.read_bytes()).hexdigest()
  receipt=self.home/'.config/SteamFrameNativePassthrough/install.json';receipt.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
  receipt.write_text(json.dumps({'schema':'sfnp-user-install-v1','home':str(self.home),'data':str(data),'manifest_link':str(pathlib.Path('/run/user')/str(os.getuid())/'steam-frame-native-passthrough/receiver.json'),'files':files,'package_sha256':hashlib.sha256((root/'package.json').read_bytes()).hexdigest()}));receipt.chmod(0o600)
  return {'reason':'installed'}
 def uninstall(self):
  receipt=self.home/'.config/SteamFrameNativePassthrough/install.json'
  if receipt.exists():
   for name,pin in json.loads(receipt.read_bytes())['files'].items():
    p=pathlib.Path(name)
    if p.exists():
     if hashlib.sha256(p.read_bytes()).hexdigest()!=pin:raise ValueError('unknown changed file')
     p.unlink()
   receipt.unlink()
  return {'reason':'uninstalled'}
'''


class System:
    def __init__(self, registry):
        self.registry=registry;self.calls=[];self.fail_add=False;self.live=False
    def runtime_running(self):return self.live
    def run(self, args, data=None, **kwargs):
        self.calls.append(list(args))
        command=args[1]
        value=s.parse(s.read(self.registry))
        drivers=value.setdefault('external_drivers',[])
        if command=='adddriver':
            if self.fail_add:raise s.SetupError('fixture registration failure')
            if args[2] not in drivers:drivers.append(args[2])
        elif command=='removedriver':
            if args[2] in drivers:drivers.remove(args[2])
        elif command!='finddriver':raise AssertionError('unexpected system action')
        if command!='finddriver':s.put(self.registry,s.encoded(value),s.read(self.registry))
        return 0,b''


class Frame:
    def __init__(self, home):
        self.home=home;self.commands=[];self.namespace={'__name__':'sfnp_fixture_agent'}
        exec(compile(s.REMOTE_AGENT,'<actual-remote-agent>','exec'),self.namespace)
        # Isolate the remote CPU identity; never mutate the PC's platform module.
        self.namespace['platform']=types.SimpleNamespace(machine=lambda:'aarch64')
    def call(self, request):
        self.commands.append(request['command'])
        return self.namespace['run'](request,self.home)


class WizardTests(unittest.TestCase):
    def setUp(self):
        previous_umask=os.umask(0o077)
        self.addCleanup(os.umask,previous_umask)
        self.temp=tempfile.TemporaryDirectory(prefix='sfnp-wizard-fixture-')
        self.base=Path(self.temp.name);self.home=self.base/'PC user';self.home.mkdir(mode=0o700)
        self.framehome=self.base/'Frame user';self.framehome.mkdir(mode=0o700)
        self.env=mock.patch.dict(os.environ,{'LOCALAPPDATA':str(self.home/'AppData/Local'),
                                           'XDG_DATA_HOME':'','XDG_CONFIG_HOME':''})
        self.env.start();self.addCleanup(self.env.stop)
        self.runtime=b'owned fake runtime, not a hardware identity'
        s.put(self.framehome/'runtime.bin',self.runtime)
        self.steamvr=self.base/'Steam library with spaces/SteamVR';self.steamvr.mkdir(parents=True)
        self.tool=self.steamvr/'bin/win64/vrpathreg.exe';s.put(self.tool,b'fixture tool, never executed')
        self.local=s.Local(self.home,'Windows',steamvr=self.steamvr)
        s.put(self.local.registry,s.encoded({'runtime':[str(self.steamvr)],'external_drivers':[]}))
        self.system=System(self.local.registry);self.local.runner=self.system
        self.frame=Frame(self.framehome)
        self.bundle_root=self.base/'Bundle with spaces';self.bundle_root.mkdir()
        self.make_bundle()
    def tearDown(self):
        if hasattr(builtins,'sfnp_fixture_verified'):del builtins.sfnp_fixture_verified
        self.temp.cleanup()
    def make_bundle(self, readiness='release', verifier=VERIFIER, component=True):
        proto={'major':1,'minor':0,'wire_sha256':'a'*64}
        profile={'profile':'owned-fixture','complete':True,'architecture':'aarch64','runtime_files':{
            'fixture':{'location':'home','path':'runtime.bin','bytes':len(self.runtime),'sha256':s.sha(self.runtime)}}}
        artifacts={'compatibility.json':s.encoded(profile),'scene-presenter.spv':b'fake shader',
                   'libVkLayer_frame_pixel_snapshot.so':b'owned fake native library',
                   'tools/prepare_native_release.py':verifier,'source-manifest.json':b'{}\n'}
        if component:artifacts['bridge-component.json']=s.encoded({'schema':'sfnp-bridge-component-v1',
            'role':'receiver','bridge_protocol':proto,'provider':'vrlink-complete-au-v1'})
        pins=lambda files:{n:{'bytes':len(b),'sha256':s.sha(b)}for n,b in files.items()}
        package={'schema':'sfnp-native-package-v1','profile':profile['profile'],'build_marker':'OWNED_FIXTURE',
                 'files':pins(artifacts),'source_snapshot_sha256':s.sha(artifacts['source-manifest.json'])}
        framefiles={'frame-package/'+n:b for n,b in artifacts.items()}
        framefiles['frame-package/package.json']=s.encoded(package)
        framefiles['frame-package/tools/native_passthrough_setup.py']=NATIVE_SETUP
        pcfiles={'pc-provider/driver.vrdrivermanifest':s.encoded({'name':s.DRIVER_NAME}),
                 'pc-provider/bin/win64/driver_fixture.dll':b'owned fixture bytes'}
        self.manifest={'schema':s.SCHEMA,'readiness':readiness,'bridge_protocol':proto,'platforms':{
            'windows-x86_64':{'reviewed':True,'native_bridge':True,'role':'producer','architecture':'x86_64',
                'bridge_protocol':proto,'provider_root':'pc-provider','provider_manifest':'pc-provider/driver.vrdrivermanifest',
                'provider_registration':'user','files':pins(pcfiles),'vrpathreg':{'path':'bin/win64/vrpathreg.exe',
                    'bytes':self.tool.stat().st_size,'sha256':s.sha(self.tool.read_bytes())}}},
            'frame':{'reviewed':True,'native_bridge':True,'role':'receiver','architecture':'aarch64','bridge_protocol':proto,
                'package_root':'frame-package','setup_entry':'frame-package/tools/native_passthrough_setup.py',
                'files':pins(framefiles),'runtime_files':{'fixture':{'base':'home','path':'runtime.bin',
                    'bytes':len(self.runtime),'sha256':s.sha(self.runtime)}}}}
        for name,raw in {**pcfiles,**framefiles}.items():
            p=self.bundle_root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(raw)
        self.save_manifest()
        self.bundle=s.Bundle(self.bundle_root)
    def save_manifest(self):
        (self.bundle_root/'steam-link-bundle.json').write_bytes(s.encoded(self.manifest))
    def inventory(self):
        challenge='12'*32
        with mock.patch.object(self.frame.namespace['platform'],'machine',return_value='aarch64'):
            info=self.frame.call(s.inventory_request(self.bundle,challenge))
        return self.local.inventory(),info,challenge
    def apply(self, action='install'):
        pc,frame,challenge=self.inventory()
        return s.apply(action,self.bundle,self.local,self.frame,pc,frame,challenge)
    def snapshot(self):
        return {str(p.relative_to(self.base)):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}

    def test_cold_bundle_and_remote_payload_first_reads(self):
        # Model freshly extracted files: no pin-generating read after resetting atime.
        for path in self.bundle_root.rglob('*'):
            if path.is_file():
                info=path.stat();os.utime(path,ns=(1,info.st_mtime_ns))
        fresh=s.Bundle(self.bundle_root)
        self.assertEqual(fresh.value,self.bundle.value)
        path=self.framehome/'first-read.bin';payload=b'cold remote payload'
        path.write_bytes(payload);info=path.stat();os.utime(path,ns=(1,info.st_mtime_ns))
        self.assertEqual(self.frame.namespace['raw'](path),payload)

    def test_readers_ignore_only_atime_and_reject_identity_changes(self):
        path=self.framehome/'identity.bin';payload=b'actual file bytes'
        path.write_bytes(payload)
        fields=('st_dev','st_ino','st_mode','st_nlink','st_uid','st_gid','st_size',
                'st_mtime_ns','st_ctime_ns','st_atime_ns')
        actual_fstat=os.fstat
        def observed(descriptor, changed):
            info=actual_fstat(descriptor)
            values={field:getattr(info,field) for field in fields}
            values[changed]+=1
            return types.SimpleNamespace(**values)
        for reader in (s.read,self.frame.namespace['raw']):
            with self.subTest(reader=reader.__name__,field='st_atime_ns'):
                with mock.patch.object(os,'fstat',side_effect=lambda fd:observed(fd,'st_atime_ns')):
                    self.assertEqual(reader(path),payload)
            for field in fields[:-1]:
                with self.subTest(reader=reader.__name__,field=field):
                    with mock.patch.object(os,'fstat',side_effect=lambda fd,key=field:observed(fd,key)):
                        with self.assertRaises(ValueError):reader(path)

    def test_readers_refuse_actual_midread_replacement_or_mutation(self):
        actual_fstat=os.fstat
        for reader in (s.read,self.frame.namespace['raw']):
            for change in ('mtime','replacement'):
                with self.subTest(reader=reader.__name__,change=change):
                    path=self.framehome/'changing.bin';path.write_bytes(b'original bytes')
                    def changed(descriptor):
                        before=actual_fstat(descriptor)
                        if change=='mtime':
                            os.utime(path,ns=(before.st_atime_ns,before.st_mtime_ns+1000000000))
                        else:
                            other=path.with_suffix('.new');other.write_bytes(b'replaced bytes');os.replace(other,path)
                        return before
                    with mock.patch.object(os,'fstat',side_effect=changed):
                        with self.assertRaises(ValueError):reader(path)

    def test_combined_platform_inventory_shares_only_identical_producer_files(self):
        linux=copy.deepcopy(self.manifest['platforms']['windows-x86_64'])
        linux['reviewed']=False;linux['native_bridge']=False
        common='pc-provider/driver.vrdrivermanifest'
        native='pc-provider/bin/linux64/driver_fixture.so';payload=b'Linux prototype fixture'
        path=self.bundle_root/native;path.parent.mkdir(parents=True);path.write_bytes(payload)
        linux['files']={common:linux['files'][common],native:{'bytes':len(payload),'sha256':s.sha(payload)}}
        linux['vrpathreg']['path']='bin/linux64/vrpathreg'
        self.manifest['platforms']['linux-x86_64']=linux;self.save_manifest()
        combined=s.Bundle(self.bundle_root)
        self.assertEqual(combined.files[common],self.bundle.files[common])
        self.assertEqual(combined.files[native],payload)
        self.assertEqual(combined.platforms['linux-x86_64']['vrpathreg']['path'],'bin/linux64/vrpathreg')
        with self.assertRaisesRegex(s.SetupError,'Linux'):combined.admit('linux-x86_64')
        linux['vrpathreg']['path']='bin/win64/vrpathreg.exe';self.save_manifest()
        with self.assertRaisesRegex(s.SetupError,'registration tool contract'):s.Bundle(self.bundle_root)
        linux['vrpathreg']['path']='bin/linux64/vrpathreg'
        linux['files'][common]['sha256']='b'*64;self.save_manifest()
        with self.assertRaisesRegex(s.SetupError,'hash mismatch'):s.Bundle(self.bundle_root)
        linux['files'][common]=copy.deepcopy(self.manifest['platforms']['windows-x86_64']['files'][common])
        frame_name='frame-package/package.json'
        linux['files'][frame_name]=self.manifest['frame']['files'][frame_name];self.save_manifest()
        with self.assertRaisesRegex(s.SetupError,'Overlapping'):s.Bundle(self.bundle_root)

    def inspection_profile(self):
        driver=self.steamvr/'drivers/vrlink/bin/win64/driver_vrlink.dll'
        driver.parent.mkdir(parents=True);driver.write_bytes(b'fixture PC sender, never executed')
        pins={name:{'bytes':(self.steamvr/name).stat().st_size,'sha256':s.sha((self.steamvr/name).read_bytes())}
              for name in s.Inspection.PC_FILES['windows-x86_64']}
        profile={'schema':s.INSPECTION,'expected_pc':{'windows-x86_64':pins},
                 'runtime_files':copy.deepcopy(self.manifest['frame']['runtime_files'])}
        (self.bundle_root/'setup-inspection.json').write_bytes(s.encoded(profile))
        return profile

    def test_inspection_profile_is_metadata_only_with_fixed_paths(self):
        original=self.inspection_profile()
        profile=s.Inspection(self.bundle_root)
        self.assertEqual(profile.runtime_files,original['runtime_files'])
        for mutation in ('command','traversal','platform','runtime'):
            value=copy.deepcopy(original)
            if mutation=='command':value['execute']='evil'
            elif mutation=='traversal':
                pin=value['expected_pc']['windows-x86_64'].pop('bin/win64/vrpathreg.exe')
                value['expected_pc']['windows-x86_64']['../outside']=pin
            elif mutation=='platform':value['expected_pc']['any-platform']={}
            else:value['runtime_files']['fixture']['base']='command'
            (self.bundle_root/'setup-inspection.json').write_bytes(s.encoded(value))
            with self.subTest(mutation=mutation),self.assertRaises(s.SetupError):s.Inspection(self.bundle_root)

    def test_inspect_check_hashes_actual_files_and_never_reads_pairings(self):
        self.inspection_profile()
        for home in (self.framehome,self.local.root):
            directory=home/'.config/steam-link-native-passthrough/pairings' if home==self.framehome else home/'pairings'
            s.put(directory/'active.json',b'not even valid JSON; must not read')
        s.put(self.framehome/'.config/SteamFrameNativePassthrough/install.json',b'{}\n')
        before=self.snapshot();output=io.StringIO()
        self.frame.namespace['module']=lambda *_:self.fail('inspection must not import a script')
        with contextlib.redirect_stdout(output),mock.patch.object(self.local,'active_pair',side_effect=AssertionError('no key lookup')):
            code=s.main(['--inspect','--check','--bundle',str(self.bundle_root),'--frame','steamframe.local'],
                        local=self.local,remote_factory=lambda *_,**__:self.frame)
        self.assertEqual(code,0);self.assertEqual(before,self.snapshot());self.assertEqual(self.system.calls,[])
        value=json.loads(output.getvalue());self.assertEqual(value['schema'],s.INSPECTION)
        self.assertTrue(value['inspection_only']);self.assertFalse(value['bridge_available'])
        self.assertTrue(value['frame']['runtime_compatible']);self.assertTrue(value['frame']['legacy_installed'])
        self.assertEqual({item['status'] for item in value['pc']['files'].values()},{'match'})
        self.assertEqual(self.frame.commands,['inventory-inspect']);self.assertNotIn('pair',output.getvalue())
        driver=self.steamvr/'drivers/vrlink/bin/win64/driver_vrlink.dll';driver.write_bytes(b'changed')
        observed=s.Inspection(self.bundle_root).pc_files(self.local.inventory(inspection=True))
        self.assertEqual(observed[str(driver.relative_to(self.steamvr))]['status'],'different')
        driver.unlink()
        self.assertEqual(s.Inspection(self.bundle_root).pc_files(self.local.inventory(inspection=True))[
                         str(driver.relative_to(self.steamvr))]['status'],'missing')

    def test_interactive_inspection_uses_manual_ssh_fallback_and_saves_only_pc_report(self):
        self.inspection_profile();before={str(p.relative_to(self.framehome)):p.read_bytes()for p in self.framehome.rglob('*')if p.is_file()}
        attempts=[];prompts=[];answers=iter(('n','192.168.1.5',''))
        def factory(address,login,interactive=False):
            attempts.append((address,login,interactive))
            if not interactive:raise s.SetupError('SSH probe unavailable')
            return self.frame
        def answer(prompt):prompts.append(prompt);return next(answers)
        output=io.StringIO()
        with contextlib.redirect_stdout(output),mock.patch('sys.stdin.isatty',return_value=True), \
             mock.patch.object(s,'apply',side_effect=AssertionError('never apply')):
            code=s.main(['--inspect','--bundle',str(self.bundle_root)],local=self.local,
                        remote_factory=factory,input_fn=answer)
        self.assertEqual(code,0);self.assertEqual(attempts[-1],('192.168.1.5','steamos',True))
        self.assertEqual(self.frame.commands,['inventory-inspect']);self.assertEqual(self.system.calls,[])
        self.assertFalse(any('Choose' in prompt or 'Apply' in prompt for prompt in prompts))
        self.assertEqual(before,{str(p.relative_to(self.framehome)):p.read_bytes()for p in self.framehome.rglob('*')if p.is_file()})
        files=[p.relative_to(self.local.root).as_posix()for p in self.local.root.rglob('*')if p.is_file()]
        self.assertEqual(files,['setup-inspection.json'])
        report=s.parse(s.read(self.local.root/'setup-inspection.json',private=True))
        self.assertTrue(report['inspection_only']);self.assertNotIn('pair',json.dumps(report))
        self.assertIn('new bridge is unavailable',output.getvalue())
        self.assertIn('Only a PC inspection report',output.getvalue())

    def test_inspect_check_without_frame_never_opens_ssh_or_saves(self):
        self.inspection_profile();before=self.snapshot();output=io.StringIO()
        with contextlib.redirect_stdout(output),mock.patch.object(s,'Bundle',side_effect=AssertionError('no payload loading')):
            code=s.main(['--inspect','--check','--bundle',str(self.bundle_root)],local=self.local,
                        remote_factory=lambda *_,**__:self.fail('no SSH without address'))
        self.assertEqual(code,0);self.assertEqual(before,self.snapshot())
        value=json.loads(output.getvalue());self.assertIsNone(value['frame']);self.assertTrue(value['inspection_only'])
        self.assertFalse(self.local.root.exists())

    def test_inspect_rejects_action_before_inventory_or_changes(self):
        before=self.snapshot();output=io.StringIO()
        with contextlib.redirect_stderr(output),mock.patch.object(self.local,'inventory',side_effect=AssertionError('no inventory')):
            code=s.main(['--inspect','--action','install','--bundle',str(self.bundle_root)],local=self.local)
        self.assertEqual(code,2);self.assertIn('cannot be combined',output.getvalue());self.assertEqual(before,self.snapshot())

    def test_inspect_dry_run_and_unknown_report_are_preserved(self):
        self.inspection_profile();before=self.snapshot();output=io.StringIO()
        with contextlib.redirect_stdout(output):
            code=s.main(['--inspect','--dry-run','--bundle',str(self.bundle_root),'--frame','steamframe.local'],
                        local=self.local,remote_factory=lambda *_,**__:self.frame)
        self.assertEqual(code,0);self.assertEqual(before,self.snapshot())
        path=self.local.root/'setup-inspection.json';foreign=b'{"schema":"foreign"}\n';s.put(path,foreign)
        error=io.StringIO()
        with contextlib.redirect_stdout(output),contextlib.redirect_stderr(error):
            code=s.main(['--inspect','--bundle',str(self.bundle_root)],local=self.local)
        self.assertEqual(code,2);self.assertEqual(path.read_bytes(),foreign)
        self.assertIn('Unknown inspection report preserved',error.getvalue())

    def test_inspect_raw_remote_failure_is_not_disclosed_or_applied(self):
        self.inspection_profile();before=self.snapshot();error=io.StringIO()
        class Failure:
            def call(self,_):raise OSError('secret password private raw SSH output')
        with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(error):
            code=s.main(['--inspect','--check','--bundle',str(self.bundle_root),'--frame','steamframe.local'],
                        local=self.local,remote_factory=lambda *_,**__:Failure())
        self.assertEqual(code,2);self.assertNotIn('secret password',error.getvalue());self.assertEqual(before,self.snapshot())

    def test_remote_generated_data_is_real_json(self):
        raw=self.frame.namespace['data']({'test':1})
        self.assertTrue(raw.endswith(b'\n'));self.assertFalse(raw.endswith(b'\\n'))
        self.assertEqual(json.loads(raw),{'test':1})
    def test_check_is_readonly_both_sides(self):
        before=self.snapshot();pc,frame,challenge=self.inventory()
        self.assertTrue(frame['runtime_compatible']);self.assertFalse(frame['installed'])
        self.assertFalse(pc['installed']);self.assertEqual(before,self.snapshot())
        self.assertFalse(self.local.root.exists());self.assertEqual(self.system.calls,[])
    def test_apply_executes_real_remote_agent_and_private_pairing(self):
        result=self.apply();self.assertEqual(result['reason'],'current')
        self.assertTrue(getattr(builtins,'sfnp_fixture_verified'))
        key=self.local.pairing(result['pair_id'])
        remote=self.frame.namespace['load'](self.framehome/'.config/steam-link-native-passthrough/pairings'/(result['pair_id']+'.json'))
        self.assertEqual(key['key_hex'],remote['key_hex']);self.assertEqual(remote['role'],'receiver')
        self.assertEqual(len(bytes.fromhex(key['key_hex'])),32)
        self.assertEqual(stat.S_IMODE((self.local.root/'pairings'/(result['pair_id']+'.json')).stat().st_mode),0o600)
        self.assertFalse((self.framehome/'.config/steam-link-native-passthrough/pairings/transaction.json').exists())
        self.assertEqual(list((self.framehome/'.local/share/steam-link-native-passthrough/staging').rglob('*')),[])
        self.assertTrue(self.local.inventory()['registered'])
    def test_repeat_is_true_noop_and_preserves_keys(self):
        first=self.apply();before=self.snapshot();calls=len(self.system.calls)
        second=self.apply('repair')
        self.assertFalse(second['changed']);self.assertEqual(first['pair_id'],second['pair_id'])
        self.assertEqual(before,self.snapshot());self.assertEqual(calls,len(self.system.calls))
    def test_installed_artifacts_are_rehashed_before_repeat_noop(self):
        self.apply()
        library=self.framehome/'.local/share/SteamFrameNativePassthrough/package/libVkLayer_frame_pixel_snapshot.so'
        for mode in ('corrupt','missing'):
            with self.subTest(mode=mode):
                if mode=='corrupt':s.put(library,b'changed actual library',s.read(library))
                else:library.unlink()
                before=self.snapshot();pc,frame,_=self.inventory()
                self.assertTrue(frame['owned_receipt_valid']);self.assertFalse(frame['files_current'])
                self.assertEqual(before,self.snapshot());self.assertIn('Repair',s.plan(pc,frame,self.bundle))
                calls=self.frame.commands.count('apply');result=self.apply('repair')
                self.assertNotEqual(result.get('changed'),False)
                self.assertEqual(self.frame.commands.count('apply'),calls+1)
                self.assertTrue(self.inventory()[1]['files_current'])
    def test_installed_helper_is_also_checked_without_import(self):
        self.apply()
        helper=self.framehome/'.local/share/SteamFrameNativePassthrough/tools/native_passthrough_setup.py'
        s.put(helper,b'raise RuntimeError("must not be imported")\n',s.read(helper))
        before=self.snapshot();frame=self.inventory()[1]
        self.assertFalse(frame['files_current']);self.assertEqual(before,self.snapshot())
    def test_prototype_never_stages_or_pairs(self):
        self.make_bundle('prototype');before=self.snapshot()
        with self.assertRaisesRegex(s.SetupError,'Prototype'):self.apply()
        self.assertEqual(before,self.snapshot());self.assertNotIn('apply',self.frame.commands)
    def test_old_public_scene_atlas_is_not_new_receiver(self):
        self.make_bundle(component=False);before=self.snapshot()
        with self.assertRaisesRegex(s.SetupError,'legacy'):self.apply()
        self.assertEqual(before,self.snapshot())
    def test_readonly_recognizes_legacy_separately(self):
        s.put(self.framehome/'.config/SteamFrameNativePassthrough/install.json',b'{}\n')
        pc,frame,_=self.inventory()
        self.assertTrue(frame['legacy_installed']);self.assertIsNone(frame['bridge_component'])
        self.assertFalse(pc['installed'])
    def test_runtime_mismatch_no_write(self):
        p=self.framehome/'runtime.bin';s.put(p,b'changed runtime',s.read(p));before=self.snapshot()
        with self.assertRaisesRegex(s.SetupError,'compatible Frame'):self.apply()
        self.assertEqual(before,self.snapshot());self.assertNotIn('apply',self.frame.commands)
    def test_local_tool_pin_failure_precedes_remote_staging(self):
        s.put(self.tool,b'changed tool',s.read(self.tool));before=self.snapshot()
        with self.assertRaisesRegex(s.SetupError,'registration tool'):self.apply()
        self.assertEqual(before,self.snapshot());self.assertNotIn('apply',self.frame.commands)
    def test_real_remote_verifier_failure_cleans_stage_before_activation(self):
        self.make_bundle(verifier=b'def verify_package(root):raise ValueError("fixture refuse")\n')
        with self.assertRaises(ValueError):self.apply()
        self.assertFalse(self.local.root.exists())
        self.assertFalse((self.framehome/'.config/SteamFrameNativePassthrough/install.json').exists())
        self.assertFalse((self.framehome/'.config/steam-link-native-passthrough/pairings').exists())
        self.assertFalse(any((self.framehome/'.local/share/steam-link-native-passthrough/staging').iterdir()))
    def test_registration_failure_rolls_pairing_back_but_reports_frame_delta(self):
        self.system.fail_add=True
        with self.assertRaises(s.SetupError):self.apply()
        self.assertIsNone(self.local.active_pair())
        pairdir=self.framehome/'.config/steam-link-native-passthrough/pairings'
        self.assertFalse((pairdir/'active.json').exists());self.assertFalse((pairdir/'transaction.json').exists())
        self.assertFalse(list(pairdir.glob('[a-f0-9]'*32+'.json')))
        # The original native setup succeeded; never claim its installation vanished.
        self.assertTrue((self.framehome/'.config/SteamFrameNativePassthrough/install.json').exists())
    def test_changed_owned_payload_and_unknown_files_preserved(self):
        self.apply();p=self.local.root/'pc-provider/foreign.txt';s.put(p,b'user file')
        pc,_,_=self.inventory()
        with self.assertRaisesRegex(s.SetupError,'Unknown provider'):self.local.install(self.bundle,pc)
        self.assertEqual(p.read_bytes(),b'user file')
        changed=self.local.root/'pc-provider/bin/win64/driver_fixture.dll'
        s.put(changed,b'unknown bytes',s.read(changed))
        with self.assertRaises(s.SetupError):self.local.uninstall(self.local.inventory())
        self.assertEqual(changed.read_bytes(),b'unknown bytes')
    def test_explicit_repair_preserves_previous_pairing_key(self):
        old=self.apply()['pair_id'];localkey=self.local.root/'pairings'/(old+'.json');previous=localkey.read_bytes()
        active=self.framehome/'.config/steam-link-native-passthrough/pairings/active.json';active.unlink()
        with self.assertRaisesRegex(s.SetupError,'mismatch'):self.apply('update')
        repaired=self.apply('repair')['pair_id'];self.assertNotEqual(old,repaired)
        self.assertEqual(localkey.read_bytes(),previous)
    def test_uninstall_removes_only_owned_provider_and_keeps_credentials(self):
        pairid=self.apply()['pair_id'];userfile=self.home/'unrelated.txt';s.put(userfile,b'keep')
        self.assertEqual(self.apply('uninstall')['reason'],'uninstalled')
        self.assertFalse(self.local.inventory()['registered']);self.assertIsNone(self.local.state())
        self.assertEqual(userfile.read_bytes(),b'keep');self.assertIsNotNone(self.local.pairing(pairid))
    def test_changed_runtime_does_not_block_owned_uninstall(self):
        self.apply()
        runtime=self.framehome/'runtime.bin';s.put(runtime,b'new runtime version',s.read(runtime))
        pc,frame,challenge=self.inventory();self.assertFalse(frame['runtime_compatible'])
        self.assertTrue(frame['owned_receipt_valid']);self.assertIn('Uninstall',s.plan(pc,frame,self.bundle))
        original=self.frame.namespace['module']
        def checked_removal(path,name):
            if path.name=='prepare_native_release.py':raise AssertionError('removal must not re-admit changed runtime')
            return original(path,name)
        self.frame.namespace['module']=checked_removal
        result=s.apply('uninstall',self.bundle,self.local,self.frame,pc,frame,challenge)
        self.assertEqual(result['reason'],'uninstalled');self.assertFalse(self.local.inventory()['registered'])
        self.assertEqual(runtime.read_bytes(),b'new runtime version')
    def test_invalid_ownership_blocks_uninstall_without_staging(self):
        self.apply()
        receipt=self.framehome/'.config/SteamFrameNativePassthrough/install.json'
        s.put(receipt,b'{"schema":"foreign"}\n',s.read(receipt));before=self.snapshot()
        calls=self.frame.commands.count('apply')
        with self.assertRaisesRegex(s.SetupError,'emergency-off'):self.apply('uninstall')
        self.assertEqual(before,self.snapshot());self.assertEqual(calls,self.frame.commands.count('apply'))
    def test_native_uninstall_pending_does_not_claim_complete_removal(self):
        self.apply()
        original=self.frame.namespace['module']
        def pending(path,name):
            module=original(path,name)
            if path.name=='native_passthrough_setup.py':
                module.Setup.uninstall=lambda self:{'reason':'uninstall_pending'}
            return module
        self.frame.namespace['module']=pending
        with self.assertRaisesRegex(s.SetupError,'emergency-off.*no complete removal'):self.apply('uninstall')
        self.assertTrue(self.local.inventory()['registered']);self.assertIsNotNone(self.local.state())
    def test_linux_refuses_windows_provider(self):
        pc,frame,challenge=self.inventory();pc['platform']='linux-x86_64';before=self.snapshot()
        with self.assertRaisesRegex(s.SetupError,'Linux'):s.apply('install',self.bundle,self.local,self.frame,pc,frame,challenge)
        self.assertEqual(before,self.snapshot())
    def test_manifest_traversal_hash_role_protocol_and_duplicate_refused(self):
        for name in ('../bad','/bad','pc-provider/../../bad','pc-provider\\bad','pc-provider/C:bad'):
            with self.subTest(name=name),self.assertRaises(s.SetupError):s.relative(name)
        original=copy.deepcopy(self.manifest)
        for field,value in [('role','receiver'),('architecture','arm64'),('bridge_protocol',{'major':9})]:
            self.manifest=copy.deepcopy(original);self.manifest['platforms']['windows-x86_64'][field]=value;self.save_manifest()
            with self.subTest(field=field),self.assertRaises(s.SetupError):s.Bundle(self.bundle_root)
        self.manifest=original;self.save_manifest()
        p=self.bundle_root/'pc-provider/bin/win64/driver_fixture.dll';p.write_bytes(b'wrong')
        with self.assertRaisesRegex(s.SetupError,'hash mismatch'):s.Bundle(self.bundle_root)
        with self.assertRaisesRegex(s.SetupError,'Duplicate'):s.parse(b'{"x":1,"x":2}')
    def test_host_user_and_discovery_bounds(self):
        for value in ('-oProxyCommand=evil','x;touch bad','user@host','host:22','x\ny','../host','x`id`'):
            with self.subTest(host=value),self.assertRaises(s.SetupError):s.host(value)
        self.assertEqual(s.host('192.168.1.5'),'192.168.1.5');self.assertEqual(s.host('steamframe.local'),'steamframe.local')
        for value in ('root;id','user@host','--bad','User Name'):
            with self.assertRaises(s.SetupError):s.user(value)
        for cidr in ('8.8.8.0/24','10.0.0.0/8','192.168.1.0/23'):
            with self.assertRaises((ValueError,s.SetupError)):s.discover(cidr,lambda *_:False)
        calls=[]
        found=s.discover('192.168.1.0/30',lambda address,port,timeout:calls.append((address,port,timeout)) or address.endswith('.2'))
        self.assertEqual(found,['192.168.1.2']);self.assertEqual(len(calls),2)
    def test_ssh_defaults_preserve_fingerprint_and_key_flow(self):
        args=s.ssh_args('steamframe.local','steamos')
        self.assertIn('StrictHostKeyChecking=yes',args);self.assertIn('BatchMode=yes',args)
        self.assertIn('UpdateHostKeys=no',args);self.assertIn('ControlMaster=no',args)
        interactive=s.ssh_args('192.168.1.2','steamos',interactive=True)
        self.assertIn('StrictHostKeyChecking=ask',interactive);self.assertNotIn('StrictHostKeyChecking=no',interactive)
        self.assertNotIn('password',str(args).lower())
    def test_runner_stdin_and_output_limits_use_real_child_processes(self):
        runner=s.Runner()
        code,raw=runner.run([sys.executable,'-B','-c','import sys;print(len(sys.stdin.buffer.read()))'],b'fixture private input')
        self.assertEqual((code,raw),(0,b'21\n'))
        with self.assertRaisesRegex(s.SetupError,'output bound'):
            runner.run([sys.executable,'-B','-c','import sys;sys.stdout.write("x"*200000)'])
        with self.assertRaisesRegex(s.SetupError,'timed out'):
            runner.run([sys.executable,'-B','-c','import time;time.sleep(5)'],timeout=.05)
    def test_public_report_never_exposes_pairing_proof_or_key(self):
        self.apply();pc,frame,challenge=self.inventory()
        key=self.local.pairing(pc['active_pair'])['key_hex'];report=json.dumps(s.public_report(pc,frame,self.bundle))
        self.assertNotIn(key,report);self.assertNotIn('pair_proofs',report);self.assertNotIn(challenge,report)
    def test_check_cli_does_not_write_log_cache_or_credentials(self):
        before=self.snapshot();output=io.StringIO()
        def factory(*_,**__):return self.frame
        with contextlib.redirect_stdout(output),mock.patch.object(self.frame.namespace['platform'],'machine',return_value='aarch64'):
            code=s.main(['--check','--bundle',str(self.bundle_root),'--frame','steamframe.local'],local=self.local,remote_factory=factory)
        self.assertEqual(code,0);self.assertEqual(before,self.snapshot())
        result=json.loads(output.getvalue());self.assertFalse(result['automatic_restart'])
    def test_remote_pairing_failure_journal_undo_executes_real_agent(self):
        original=self.frame.namespace['put']
        def fail_active(path,raw,old=None):
            if path.name=='active.json':raise ValueError('fixture write failure')
            return original(path,raw,old)
        self.frame.namespace['put']=fail_active
        with self.assertRaises(ValueError):self.apply()
        pairdir=self.framehome/'.config/steam-link-native-passthrough/pairings'
        self.assertFalse((pairdir/'transaction.json').exists());self.assertFalse((pairdir/'active.json').exists())
        self.assertFalse(list(pairdir.glob('[a-f0-9]'*32+'.json')))
    def test_symlink_inputs_and_foreign_receipts_refused(self):
        p=self.bundle_root/'pc-provider/bin/win64/driver_fixture.dll';original=p.read_bytes();p.unlink()
        target=self.base/'other.dll';target.write_bytes(original);p.symlink_to(target)
        with self.assertRaises(s.SetupError):s.Bundle(self.bundle_root)
        s.put(self.local.root/'install.json',b'{"schema":"foreign"}\n')
        with self.assertRaises(s.SetupError):self.local.inventory()
    def test_unknown_log_is_preserved(self):
        foreign=b'{"schema":"foreign-log","action":"keep"}\n'
        path=self.local.root/'setup-last.log';s.put(path,foreign)
        with self.assertRaises(s.SetupError):s.write_report(self.local,'repair',{},failed=True)
        self.assertEqual(path.read_bytes(),foreign)
    def test_address_cache_requires_owned_schema(self):
        foreign=b'{"host":"unrelated.local","user":"steamos"}\n'
        path=self.local.root/'frame-address.json';s.put(path,foreign)
        with self.assertRaises(s.SetupError):s.remember_frame(self.local,'steamframe.local','steamos')
        self.assertEqual(path.read_bytes(),foreign)
        path.unlink();s.remember_frame(self.local,'steamframe.local','steamos')
        before=path.stat();s.remember_frame(self.local,'steamframe.local','steamos')
        self.assertEqual(before,path.stat())
    def test_partial_apply_failure_has_redacted_log_after_admission(self):
        self.system.fail_add=True
        def factory(*_,**__):return self.frame
        output,error=io.StringIO(),io.StringIO()
        with contextlib.redirect_stdout(output),contextlib.redirect_stderr(error), \
             mock.patch('sys.stdin.isatty',return_value=True), \
             mock.patch.object(self.frame.namespace['platform'],'machine',return_value='aarch64'):
            code=s.main(['--action','install','--bundle',str(self.bundle_root),'--frame','steamframe.local'],
                        local=self.local,remote_factory=factory,input_fn=lambda _:'y')
        self.assertEqual(code,2)
        raw=s.read(self.local.root/'setup-last.log');value=s.parse(raw)
        self.assertEqual(value['schema'],'sfnp-wizard-log-v1');self.assertTrue(value['failed'])
        self.assertIn('may already be installed',value['recovery'])
        self.assertNotIn(b'key_hex',raw);self.assertNotIn(b'pair_proofs',raw)
    def test_safe_actionable_linux_refusal_is_visible(self):
        self.local.system='Linux'
        def factory(*_,**__):return self.frame
        output,error=io.StringIO(),io.StringIO();before=self.snapshot()
        with contextlib.redirect_stdout(output),contextlib.redirect_stderr(error), \
             mock.patch('sys.stdin.isatty',return_value=True), \
             mock.patch.object(self.frame.namespace['platform'],'machine',return_value='aarch64'):
            code=s.main(['--action','install','--bundle',str(self.bundle_root),'--frame','steamframe.local'],
                        local=self.local,remote_factory=factory,input_fn=lambda _:'y')
        self.assertEqual(code,2);self.assertIn('Native Linux sender provider is not reviewed',error.getvalue())
        self.assertEqual(before,self.snapshot())
    def test_untrusted_system_error_is_not_printed_or_logged_during_check(self):
        class Failure:
            def call(self,_):raise OSError('fixture secret password must not be logged')
        output,error=io.StringIO(),io.StringIO();before=self.snapshot()
        self.local.apply_started=True # A reused caller cannot make --check write.
        with contextlib.redirect_stdout(output),contextlib.redirect_stderr(error):
            code=s.main(['--check','--bundle',str(self.bundle_root),'--frame','steamframe.local'],
                        local=self.local,remote_factory=lambda *_,**__:Failure())
        self.assertEqual(code,2);self.assertNotIn('secret password',error.getvalue()+output.getvalue())
        self.assertEqual(before,self.snapshot())


if __name__=='__main__':unittest.main()
