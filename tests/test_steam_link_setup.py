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
NATIVE_SETUP = b'''import builtins,contextlib,hashlib,json
class Setup:
 def __init__(self,home):self.home=home
 @contextlib.contextmanager
 def locked(self):yield
 def install(self,root):
  if not getattr(builtins,'sfnp_fixture_verified',False):raise ValueError('verifier must precede activation')
  receipt=self.home/'.config/SteamFrameNativePassthrough/install.json';receipt.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
  receipt.write_text(json.dumps({'package_sha256':hashlib.sha256((root/'package.json').read_bytes()).hexdigest()}));receipt.chmod(0o600)
  component=self.home/'.local/share/SteamFrameNativePassthrough/package/bridge-component.json';component.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
  component.write_bytes((root/'bridge-component.json').read_bytes());component.chmod(0o600)
  return {'reason':'installed'}
 def uninstall(self):
  receipt=self.home/'.config/SteamFrameNativePassthrough/install.json'
  if receipt.exists():receipt.unlink()
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
            info=self.frame.call({'command':'inventory','challenge':challenge,'runtime_files':self.bundle.frame['runtime_files']})
        return self.local.inventory(),info,challenge
    def apply(self, action='install'):
        pc,frame,challenge=self.inventory()
        return s.apply(action,self.bundle,self.local,self.frame,pc,frame,challenge)
    def snapshot(self):
        return {str(p.relative_to(self.base)):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}

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
        self.assertFalse(any((self.framehome/'.local/share/steam-link-native-passthrough/staging').iterdir()))
        self.assertTrue(self.local.inventory()['registered'])
    def test_repeat_is_true_noop_and_preserves_keys(self):
        first=self.apply();before=self.snapshot();calls=len(self.system.calls)
        second=self.apply('repair')
        self.assertFalse(second['changed']);self.assertEqual(first['pair_id'],second['pair_id'])
        self.assertEqual(before,self.snapshot());self.assertEqual(calls,len(self.system.calls))
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


if __name__=='__main__':unittest.main()
