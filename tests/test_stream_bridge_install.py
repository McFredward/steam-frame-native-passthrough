"""Real setup methods on isolated temporary files; no Steam/SSH/device actions."""
import base64
from contextlib import contextmanager
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
s=module('engine',ROOT/'setup/steam_link_setup.py')
b=module('bridge_installer',ROOT/'setup/stream_bridge_install.py')

class System:
    def __init__(self,registry):self.registry=registry;self.calls=[];self.live=False;self.fail_add=False
    def runtime_running(self):return self.live
    def run(self,args,**kwargs):
        self.calls.append(args)
        state=s.parse(s.read(self.registry));drivers=state.setdefault('external_drivers',[])
        if args[1]=='adddriver':
            if args[2] not in drivers:drivers.append(args[2])
        elif args[1]=='removedriver':
            if args[2] in drivers:drivers.remove(args[2])
        else:raise AssertionError(args)
        s.put(self.registry,s.encoded(state),s.read(self.registry))
        if self.fail_add and args[1]=='adddriver':raise s.SetupError('fixture registration failed after write')
        return 0,b''

class Tests(unittest.TestCase):
    def setUp(self):
        old_umask=os.umask(0o077);self.addCleanup(os.umask,old_umask)
        self.temp=tempfile.TemporaryDirectory(prefix='sfnp-install-fixture-');self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name);self.home=self.base/'pc';self.home.mkdir(mode=0o700)
        self.fhome=self.base/'frame';self.fhome.mkdir(mode=0o700)
        self.env=mock.patch.dict(os.environ,{'LOCALAPPDATA':str(self.home/'AppData/Local'),'XDG_DATA_HOME':'','XDG_CONFIG_HOME':''})
        self.env.start();self.addCleanup(self.env.stop)
        self.runtime=self.base/'SteamVR';self.runtime.mkdir(mode=0o700)
        self.local=s.Local(self.home,'Windows',steamvr=self.runtime)
        self.config=self.base/'config';self.config.mkdir(mode=0o700)
        s.put(self.local.registry,s.encoded({'runtime':[str(self.runtime)],'config':[str(self.config)],'external_drivers':[]}))
        self.system=System(self.local.registry);self.local.runner=self.system
        self.settings=self.config/'steamvr.vrsettings';s.put(self.settings,s.encoded({'camera':{'other':7},b.SETTINGS_SECTION:{'controlPort':2222}}))
        self.fsettings=self.fhome/'.local/share/Steam/config/steamvr.vrsettings';s.put(self.fsettings,s.encoded({'camera':{'forceSceneAlphaBlending':False},'user':{'quality':3}}));os.chmod(self.fsettings,0o666)
        self.froot=self.fhome/'.local/share/SteamLinkNativePassthrough/stream-bridge'
        self.fsvr=self.fhome/'SteamVR';self.fsvr.mkdir(mode=0o700)
        self.pair={'schema':s.PAIR,'pair_id':'12'*16,'key_hex':'23'*32,'role':'producer','protocol_major':1}
        self.commands=[];self.active=False
        def system(args,**kw):
            self.commands.append(args)
            if args[2:]==['show','steamvr.service','--property=ActiveState','--value']:return 0,b'active\n' if self.active else b'inactive\n'
            if args[2:]==['show','steam.service','--property=ActiveState','--value']:return 0,b'inactive\n'
            if args[2:]==['show','steam.service','--property=Environment','--value']:return 0,b'LD_PRELOAD=/foreign/a.so:/foreign/b.so LD_AUDIT=/foreign/audit.so\n'
            if args[2:]==['daemon-reload']:return 0,b''
            raise AssertionError(args)
        self.unit=b.FrameInstall(s,home=self.fhome,steamvr=self.fsvr,root=self.froot,config=self.fhome/'.config',run=system,machine=lambda:'aarch64')
        self.bundle_root=self.base/'bundle';self.bundle_root.mkdir(mode=0o700)
        self.make_bundle()

    def make_bundle(self,readiness='release'):
        pins=lambda raw:{'bytes':len(raw),'sha256':s.sha(raw)}
        elf=bytearray(80);elf[:7]=b'\x7fELF\x02\x01\x01';elf[18:20]=b'\xb7\x00'
        payloads={'pc-provider/driver.vrdrivermanifest':s.encoded({'name':b.NAME,'alwaysActivate':True}),
                  'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll':b'fixture provider, never loaded',
                  'frame/'+b.LIBRARY:bytes(elf),'frame/libsfnp_stream_launch.so':bytes(elf),
                  'frame/tools/native_fragment_stream_coverage.py':b'# fixture module\n',
                  'frame/tools/native_fragment_scene_presenter.py':b'# fixture module\n',
                  'frame/tools/compositor_startup_health.py':b'# fixture module\n'}
        for name in b.PROFILES:payloads[name]=b'{}\n'
        for name in b.TOOLS:payloads.setdefault(name,b'# owned fixture source, not native execution\n')
        pc_pins={}
        for role,name in [('vrlink','drivers/vrlink/bin/win64/driver_vrlink.dll'),('vrpathreg','bin/win64/vrpathreg.exe')]:
            raw=('fixture '+role).encode();s.put(self.runtime/name,raw);pc_pins[role]={'base':'steamvr','path':name,**pins(raw)}
        frame_pins={}
        for role,name in [('vrlink','tools/vrlink/bin/linuxarm64/vrlink'),('compositor','bin/linuxarm64/vrcompositor'),('fragment','resources/native.spv'),('openvr','bin/linuxarm64/libopenvr_api.so')]:
            raw=('frame fixture '+role).encode();s.put(self.fsvr/name,raw);frame_pins[role]={'base':'steamvr','path':name,**pins(raw)}
        self.manifest={'schema':b.SCHEMA,'readiness':readiness,'bridge_protocol':{'major':1,'minor':0,'wire_sha256':'a'*64},
                       'files':{name:pins(raw) for name,raw in payloads.items()},
                       'pc':{'reviewed':True,'platform':'windows-x86_64','files':sorted(b.PC_FILES),'runtime_files':pc_pins},
                       'frame':{'reviewed':True,'files':sorted(n for n in payloads if n.startswith('frame/')),
                                'runtime_files':frame_pins,'generated':b.GENERATED}}
        for name,raw in payloads.items():s.put(self.bundle_root/name,raw)
        s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest))
        self.bundle=b.Bundle(self.bundle_root,s)

    @contextmanager
    def fake_generation(self):
        # Only the transaction boundary is exercised here. Actual pinned shader
        # derivation is covered by the parent's separate native GPU evidence.
        with mock.patch.object(b,'generate_shaders',return_value={'coverage.spv':b'fixture coverage','legacy.spv':b'fixture legacy'}):yield

    def test_prototype_never_applies(self):
        self.manifest['readiness']='prototype';s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest),s.read(self.bundle_root/'stream-bridge-bundle.json'))
        bundle=b.Bundle(self.bundle_root,s)
        before={str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}
        with self.assertRaisesRegex(s.SetupError,'preview'):b.LocalInstall(s,self.local).install(bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        with self.assertRaisesRegex(s.SetupError,'preview'):
            with self.fake_generation():self.unit.install(bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.assertEqual(before,{str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()})

    def test_pc_install_and_restore_keys_preserve_unrelated(self):
        installer=b.LocalInstall(s,self.local);pc=self.local.inventory()
        result=installer.install(self.bundle,pc,self.pair,'192.168.1.4',47631)
        self.assertEqual(result['reason'],'installed');self.assertTrue(self.local.inventory()['registered'])
        value=s.parse(s.read(self.settings));self.assertTrue(value[b.SETTINGS_SECTION]['enableStreamBridge']);value['camera']['later']=8;s.put(self.settings,s.encoded(value),s.read(self.settings))
        result=installer.uninstall(self.local.inventory());self.assertEqual(result['reason'],'uninstalled')
        value=s.parse(s.read(self.settings));self.assertEqual(value[b.SETTINGS_SECTION],{'controlPort':2222});self.assertEqual(value['camera'],{'other':7,'later':8})
        self.assertFalse(self.local.inventory()['registered']);self.assertFalse(installer.receipt.exists())
        self.assertTrue((self.local.root/'pairings'/('12'*16+'.json')).exists())

    def test_pc_partial_registration_restores(self):
        installer=b.LocalInstall(s,self.local);original=s.read(self.settings);self.system.fail_add=True
        with self.assertRaises(s.SetupError):installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        self.assertEqual(s.read(self.settings),original);self.assertFalse(self.local.inventory()['registered']);self.assertFalse(installer.receipt.exists())
        self.assertFalse((self.local.root/'pc-provider/driver.vrdrivermanifest').exists())

    def test_changed_settings_or_payload_preserved(self):
        installer=b.LocalInstall(s,self.local);installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        value=s.parse(s.read(self.settings));value[b.SETTINGS_SECTION]['controlPort']=3333;s.put(self.settings,s.encoded(value),s.read(self.settings))
        with self.assertRaisesRegex(s.SetupError,'Changed SteamVR'):installer.uninstall(self.local.inventory())
        self.assertEqual(s.parse(s.read(self.settings))[b.SETTINGS_SECTION]['controlPort'],3333)
        self.assertTrue(installer.receipt.exists());self.assertTrue(self.local.inventory()['registered'])

    def test_pc_live_or_wrong_runtime_no_write(self):
        installer=b.LocalInstall(s,self.local);self.system.live=True
        with self.assertRaisesRegex(s.SetupError,'Close SteamVR'):installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        self.system.live=False;target=self.runtime/'drivers/vrlink/bin/win64/driver_vrlink.dll';s.put(target,b'changed',s.read(target))
        with self.assertRaisesRegex(s.SetupError,'not supported'):installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        self.assertFalse(installer.receipt.exists())

    def test_frame_real_transaction_no_system_binary_write(self):
        original=s.read(self.fsettings);elfs={str(p):p.read_bytes() for p in self.fsvr.rglob('*') if p.is_file()}
        with self.fake_generation():result=self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.assertEqual(result['reason'],'prepared');self.assertFalse(result['coverage_operational'])
        self.unit.tx.commit();self.assertTrue(self.unit.inventory(self.bundle)['files_current'])
        launch=(self.froot/'launch.conf').read_text().splitlines();self.assertEqual(launch[0],'SFNP_STREAM_LAUNCH/1');self.assertEqual(launch[1],str(self.fsvr/'tools/vrlink/bin/linuxarm64/vrlink'))
        dropin=(self.fhome/'.config/systemd/user/steam.service.d/85-steam-link-native-passthrough.conf').read_text()
        self.assertIn('LD_PRELOAD=/foreign/a.so:/foreign/b.so:',dropin);self.assertNotIn('LD_AUDIT=',dropin)
        value=s.parse(s.read(self.fsettings));value['user']['quality']=5;s.put(self.fsettings,s.encoded(value),s.read(self.fsettings))
        self.assertEqual(self.unit.undo()['reason'],'uninstalled');self.assertFalse(self.unit.tx.path.exists())
        restored=s.parse(s.read(self.fsettings));self.assertFalse(restored['camera']['forceSceneAlphaBlending']);self.assertEqual(restored['user']['quality'],5)
        self.assertEqual(elfs,{str(p):p.read_bytes() for p in self.fsvr.rglob('*') if p.is_file()})
        self.assertTrue(all(not any(x in ('start','stop','restart','try-restart') for x in cmd) for cmd in self.commands))
        self.assertNotEqual(s.read(self.fsettings),original)

    def test_frame_live_refuses_before_generation(self):
        self.active=True
        with mock.patch.object(b,'generate_shaders') as generate:
            with self.assertRaisesRegex(s.SetupError,'Stop SteamVR'):self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        generate.assert_not_called();self.assertFalse(self.unit.tx.path.exists())

    def test_frame_rollback_after_interruption(self):
        with self.fake_generation():operations,settings,_=self.unit.plan(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        real=self.unit.tx.replace
        calls=[]
        def fail(operation,reverse=False):
            calls.append(operation)
            if len(calls)==5 and not reverse:raise OSError('fixture interruption')
            return real(operation,reverse)
        with mock.patch.object(self.unit.tx,'replace',side_effect=fail):
            with self.assertRaises(OSError):self.unit.tx.begin(self.bundle.pin,operations,settings,self.pair['pair_id'])
        self.assertFalse(self.unit.tx.path.exists());self.assertFalse((self.froot/b.LIBRARY).exists())

    def test_foreign_frame_file_preserved(self):
        s.put(self.froot/b.LIBRARY,b'foreign')
        with self.fake_generation():
            with self.assertRaisesRegex(s.SetupError,'unknown Frame'):self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.assertEqual(s.read(self.froot/b.LIBRARY),b'foreign');self.assertFalse(self.unit.tx.path.exists())


    def test_legacy_activation_suspended_and_restored_exactly(self):
        data=self.fhome/'.local/share/SteamFrameNativePassthrough'
        dropin=self.fhome/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf'
        link=self.fhome/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json'
        control=self.fhome/'.config/SteamFrameNativePassthrough/control.json'
        payload=data/'package/library.so';s.put(payload,b'legacy fixture library');s.put(dropin,b'[Service]\nExecCondition=/owned/old\n')
        s.put(control,s.encoded({'schema':'sfnp-user-control-v1','enabled':True}))
        link.parent.mkdir(parents=True,mode=0o700);target='/run/user/'+str(os.getuid())+'/steam-frame-native-passthrough/receiver.json';os.symlink(target,link)
        receipt=self.fhome/'.config/SteamFrameNativePassthrough/install.json'
        s.put(receipt,s.encoded({'schema':'sfnp-user-install-v1','home':str(self.fhome),'data':str(data),'manifest_link':target,
                               'package_sha256':'a'*64,'files':{str(payload):s.sha(s.read(payload)),str(dropin):s.sha(s.read(dropin))}}))
        old_dropin=s.read(dropin);old_control=s.read(control);old_receipt=s.read(receipt)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.assertFalse(dropin.exists());self.assertFalse(os.path.lexists(link));self.assertFalse(s.parse(s.read(control))['enabled'])
        self.unit.tx.commit();self.unit.undo()
        self.assertEqual(s.read(dropin),old_dropin);self.assertEqual(s.read(control),old_control);self.assertEqual(os.readlink(link),target);self.assertEqual(s.read(receipt),old_receipt)



    def test_generation_stages_protocol_json_and_ignores_cached_tools(self):
        import types,sys
        coverage=b'owned fixture coverage';legacy=b'owned fixture legacy'
        files={name:b'# unused own fixture source\n' for name in b.TOOLS}
        files['frame/tools/native_fragment_ssa_probe.py']=b'IDENTITY="owned"\n'
        files['frame/tools/native_fragment_stream_coverage.py']=b'import native_fragment_ssa_probe as s\ndef transform(original):\n assert s.IDENTITY=="owned"\n return b"owned fixture coverage",{}\n'
        files['frame/tools/native_fragment_scene_presenter.py']=b'from pathlib import Path\ndef transform_robust(original,enabled,count):\n root=Path(__file__).resolve().parents[1]\n for version in (1,3):\n  assert (root/"docs/protocols"/("scene-contribution-atlas-v"+str(version)+".json")).read_bytes()==b"owned profile"\n return b"owned fixture legacy",{}\n'
        for name in b.PROFILES:files[name]=b'owned profile'
        bundle=types.SimpleNamespace(frame={'files':sorted(files)},files=files)
        pins={name:{'bytes':len(raw),'sha256':s.sha(raw)} for name,raw in [('coverage.spv',coverage),('legacy.spv',legacy)]}
        cached=types.ModuleType('native_fragment_ssa_probe');cached.IDENTITY='foreign';cached.__file__='/foreign/cached.py'
        previous=sys.modules.get('native_fragment_ssa_probe');sys.modules['native_fragment_ssa_probe']=cached
        try:
            with tempfile.TemporaryDirectory(prefix='sfnp-own-source-') as stage,mock.patch.object(b,'GENERATED',pins):
                result=b.generate_shaders(s,bundle,b'owned fixture input',Path(stage))
            self.assertEqual(result,{'coverage.spv':coverage,'legacy.spv':legacy});self.assertIs(sys.modules['native_fragment_ssa_probe'],cached)
        finally:
            if previous is None:sys.modules.pop('native_fragment_ssa_probe',None)
            else:sys.modules['native_fragment_ssa_probe']=previous

    def test_actual_remote_bootstrap_inventory_creates_no_frame_files(self):
        import subprocess
        request=b.request(self.bundle,'bridge-inventory')
        request['_engine']=base64.b64encode((ROOT/'setup/steam_link_setup.py').read_bytes()).decode()
        request['_bridge']=base64.b64encode((ROOT/'setup/stream_bridge_install.py').read_bytes()).decode()
        before={str(p):p.read_bytes() for p in self.fhome.rglob('*') if p.is_file()}
        env=dict(os.environ,HOME=str(self.fhome),XDG_DATA_HOME='',XDG_CONFIG_HOME='')
        result=subprocess.run([s.sys.executable,'-I','-B','-c',b.REMOTE_BOOTSTRAP],input=s.encoded(request),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20,env=env)
        self.assertEqual(result.returncode,0,result.stderr.decode())
        reply=json.loads(result.stdout);self.assertIn('architecture',reply);self.assertFalse(reply['installed'])
        self.assertEqual(before,{str(p):p.read_bytes() for p in self.fhome.rglob('*') if p.is_file()})
        self.assertNotIn('files',request)

    def test_remote_request_validates_actual_transferred_manifest(self):
        observed=b.remote_request(s,b.request(self.bundle,'bridge-inventory'),unit=self.unit)
        self.assertTrue(observed['runtime_compatible']);self.assertFalse(observed['installed'])
        changed=b.request(self.bundle,'bridge-prepare',pairing={**self.pair,'role':'receiver'},address='192.168.1.4',port=47631);changed['files']['frame/'+b.LIBRARY]['data']=base64.b64encode(b'changed').decode()
        with self.assertRaisesRegex(s.SetupError,'transferred'):b.remote_request(s,changed,unit=self.unit)
        self.assertFalse(self.unit.tx.path.exists())

    def test_cli_prototype_check_actual_dispatch_is_read_only(self):
        self.manifest['readiness']='prototype';s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest),s.read(self.bundle_root/'stream-bridge-bundle.json'))
        before={str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}
        import contextlib,io
        output=io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(s.main(['--bundle',str(self.bundle_root),'--check'],local=self.local),0)
        report=json.loads(output.getvalue());self.assertFalse(report['bridge_available']);self.assertFalse(report['automatic_restart'])
        self.assertEqual(before,{str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()})

    def test_interrupted_frame_transaction_retains_explicit_undo(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.assertEqual(self.unit.tx.state()['phase'],'pending')
        self.unit.undo();self.assertFalse(self.unit.tx.path.exists());self.assertFalse((self.froot/'receiver.enable').exists())
        self.assertFalse(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'])



    def test_repair_only_missing_owned_code_and_registration(self):
        installer=b.LocalInstall(s,self.local);installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        driver=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll';driver.unlink()
        paths=s.parse(s.read(self.local.registry));paths['external_drivers']=[];s.put(self.local.registry,s.encoded(paths),s.read(self.local.registry))
        self.assertEqual(installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)['reason'],'repaired')
        self.assertEqual(s.read(driver),self.bundle.files[str(driver.relative_to(self.local.root))]);self.assertTrue(self.local.inventory()['registered'])
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();image=self.froot/'coverage.spv';image.unlink()
        self.assertEqual(self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)['reason'],'repaired')
        self.assertEqual(s.read(image),b'fixture coverage')
        s.put(image,b'user changed',s.read(image))
        with self.assertRaisesRegex(s.SetupError,'Changed Frame'):self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.assertEqual(s.read(image),b'user changed')

    def test_loaded_selector_requires_steam_stop_before_uninstall(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();real=self.unit.run
        def running(args,**kw):
            if args[2:]==['show','steam.service','--property=ActiveState','--value']:return 0,b'active\n'
            return real(args,**kw)
        self.unit.run=running
        before=self.unit.tx.path.read_bytes()
        with self.assertRaisesRegex(s.SetupError,'Stop Steam on'):self.unit.undo()
        self.assertEqual(self.unit.tx.path.read_bytes(),before);self.assertTrue((self.froot/b.LIBRARY).exists())

    def test_failed_start_automatically_restores_files_and_running_frame(self):
        import types,contextlib
        class UI:
            def __init__(self):self.messages=[]
            def banner(self,**kw):pass
            def status(self,*a):pass
            def path(self,*a,**kw):pass
            def say(self,key):self.messages.append(key)
            def suspend(self):return contextlib.nullcontext()
            def confirm(self,key):return True
            def choose_action(self,*a,**kw):return 'exit'
        ui=UI();commands=[];health_calls=[];runtime_actions=[]
        self.active=True;self.system.live=True
        class Runtime:
            def __init__(inner,*a):pass
            def snapshot(inner):runtime_actions.append('snapshot')
            def stop(inner):runtime_actions.append('stop');self.system.live=False
            def start(inner):runtime_actions.append('start');self.system.live=True
            def restore(inner):runtime_actions.append('restore');self.system.live=True
            def finish(inner):runtime_actions.append('finish')
        class Peer:
            def call(inner,request):
                commands.append(request['command'])
                if request['command']=='bridge-health-baseline':return {'baseline':'synthetic','checkpoint':'synthetic'}
                if request['command']=='bridge-health':
                    health_calls.append(1);return {'software_ready':len(health_calls)>1,'automatic_restart':False}
                if request['command'].startswith('bridge-runtime-'):
                    self.assertTrue(self.active);return {'running':True,'needs_health_check':True}
                return b.remote_request(s,request,unit=self.unit)
        real=b.load_helper
        def helper(engine,name):return types.SimpleNamespace(Lifecycle=Runtime) if name=='steam_link_setup_pc' else real(engine,name)
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action='install')
        before=s.parse(s.read(self.fsettings))
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:ui)),mock.patch.object(b,'local_address',return_value='192.168.1.4'),mock.patch.object(b,'load_helper',side_effect=helper),self.fake_generation():
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),2)
        self.assertIn('bridge_rolled_back',ui.messages);self.assertIn('bridge-restore',commands)
        self.assertTrue(self.active);self.assertTrue(self.system.live);self.assertIn('restore',runtime_actions)
        self.assertIsNone(self.unit.tx.state());self.assertIsNone(b.LocalInstall(s,self.local).state())
        self.assertEqual(s.parse(s.read(self.fsettings)),before)
        self.assertFalse(self.unit.checkpoint().path.exists())

    def test_health_and_picture_both_required_undo_is_choice(self):
        self.assertTrue(b.offer_undo({'software_ready':False},True));self.assertTrue(b.offer_undo({'software_ready':True},False));self.assertTrue(b.offer_undo({'software_ready':True},None));self.assertFalse(b.offer_undo({'software_ready':True},True))
        health=mock.Mock();source=mock.Mock();source.baseline.return_value={'unit':'fixture'};source.checkpoint.return_value={'cursor':'fixture'};health.NativeEvidenceSource.return_value=source
        monitor=b.StartupHealth(health,self.fhome);baseline=monitor.baseline();health.wait_ready.return_value={'status':'ready','reason':None,'process_identities':{'compositor':1}}
        result=monitor.check(baseline);source.resume.assert_called_once_with({'cursor':'fixture'});self.assertTrue(result['software_ready']);self.assertFalse(result['headset_picture_verified']);self.assertFalse(result['automatic_rollback'])

    def test_remote_failure_uses_only_exact_authored_check_ids(self):
        checks=b.fixed_checks((ROOT/'setup/steam_link_setup.py').read_bytes(),(ROOT/'setup/stream_bridge_install.py').read_bytes())
        known='Stop SteamVR on the Frame before changing its loaded connection files.'
        item=b.failure_record(s,s.SetupError(known),checks)
        self.assertEqual(checks[item['reason']],known)
        for error in (s.SetupError('fixture secret password'),OSError(13,'fixture secret password'),ValueError('fixture secret password')):
            item=b.failure_record(s,error,checks)
            self.assertNotIn('fixture secret',json.dumps(item));self.assertNotIn('password',json.dumps(item))

    def test_failure_before_apply_prints_durable_log_and_recovery_paths(self):
        import contextlib,io,types
        class Peer:
            closed=False
            def call(inner,request):raise OSError(13,'fixture secret password')
            def close(inner):inner.closed=True
        peer=Peer();error=io.StringIO()
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        with contextlib.redirect_stderr(error),mock.patch.object(s.sys.stdin,'isatty',return_value=False):
            with self.assertRaises(OSError):b.run(args,s,self.local,remote_factory=lambda *a,**kw:peer)
        text=error.getvalue();self.assertIn(str(self.local.root/'logs'),text)
        self.assertIn(str(self.local.root/'setup-restore.json'),text)
        self.assertIn('SteamLinkNativePassthrough/stream-bridge/setup-restore.json',text)
        logs=list((self.local.root/'logs').glob('*'));self.assertEqual(len(logs),1)
        evidence=logs[0].read_text();self.assertIn('frame_inventory',evidence);self.assertNotIn('fixture secret',evidence)
        self.assertNotIn(self.pair['key_hex'],evidence);self.assertTrue(peer.closed)
        self.assertFalse(b.LocalInstall(s,self.local).receipt.exists())

    def test_real_remote_dispatch_reuses_one_process_without_frame_writes(self):
        import sys,subprocess,types
        wrapper=self.base/'fake-ssh.py';starts=self.base/'ssh-starts'
        wrapper.write_text('import os,pathlib,shlex,sys\n'
                           f'p=pathlib.Path({str(starts)!r});p.write_text(p.read_text()+"start\\n" if p.exists() else "start\\n")\n'
                           'args=shlex.split(sys.argv[-1]);exec(compile(args[-1],"<fake-ssh>","exec"),{"__name__":"__main__"})\n')
        remote=b.Remote(s,'192.168.1.25')
        env={'HOME':str(self.fhome),'XDG_DATA_HOME':'','XDG_CONFIG_HOME':''}
        before={str(p):p.read_bytes() for p in self.fhome.rglob('*') if p.is_file()}
        with mock.patch.dict(os.environ,env),mock.patch.object(s,'ssh_args',return_value=[sys.executable,str(wrapper)]):
            try:
                for _ in range(3):self.assertIn('architecture',remote.call(b.request(self.bundle,'bridge-inventory')))
                with self.assertRaisesRegex(s.SetupError,'Bounded package inventory required'):
                    remote.call(b.request(self.bundle,'wrong-operation'))
                self.assertIn('architecture',remote.call(b.request(self.bundle,'bridge-inventory')))
                process=remote.session._process
            finally:remote.close()
        self.assertIsNotNone(process.poll());self.assertEqual(starts.read_text(),'start\n')
        self.assertEqual(before,{str(p):p.read_bytes() for p in self.fhome.rglob('*') if p.is_file()})

    def test_manifest_rejects_missing_role_unknown_payload_and_generation(self):
        for change in ('missing','foreign','generated'):
            bad=copy.deepcopy(self.manifest)
            if change=='missing':bad['frame']['files'].remove('frame/libsfnp_stream_launch.so')
            elif change=='foreign':bad['files']['frame/native-valve.spv']={'bytes':1,'sha256':'b'*64}
            else:bad['frame']['generated']['coverage.spv']['bytes']=1
            # Generated is shared in the fixture source; restore after this vector.
            if change=='generated':
                bad['frame']['generated']=copy.deepcopy(self.manifest['frame']['generated']);bad['frame']['generated']['coverage.spv']['bytes']=1
            s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(bad),s.read(self.bundle_root/'stream-bridge-bundle.json'))
            with self.assertRaises((s.SetupError,OSError)):b.Bundle(self.bundle_root,s)
        s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest),s.read(self.bundle_root/'stream-bridge-bundle.json'))

if __name__=='__main__':unittest.main()
