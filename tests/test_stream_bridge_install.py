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
        self.env=mock.patch.dict(os.environ,{'LOCALAPPDATA':str(self.home/'AppData/Local'),'XDG_DATA_HOME':'','XDG_CONFIG_HOME':'','VR_CONFIG_PATH':'','VR_PATHREG_OVERRIDE':''})
        self.env.start();self.addCleanup(self.env.stop)
        self.runtime=self.base/'SteamVR';self.runtime.mkdir(mode=0o700)
        self.local=s.Local(self.home,'Windows',steamvr=self.runtime)
        self.config=self.base/'config';self.config.mkdir(mode=0o700)
        s.put(self.local.registry,s.encoded({'runtime':[str(self.runtime)],'config':[str(self.config)],'external_drivers':[]}))
        self.system=System(self.local.registry);self.local.runner=self.system
        self.settings=self.config/'steamvr.vrsettings';s.put(self.settings,s.encoded({'camera':{'other':7},b.SETTINGS_SECTION:{'controlPort':2222}}))
        self.fsettings=self.fhome/'.local/share/Steam/config/steamvr.vrsettings';s.put(self.fsettings,s.encoded({'camera':{'forceSceneAlphaBlending':False},'user':{'quality':3}}));os.chmod(self.fsettings,0o666)
        self.fregistry=self.fhome/'.config/openvr/openvrpaths.vrpath'
        s.put(self.fregistry,s.encoded({'config':[str(self.fsettings.parent)]}))
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
        self.camera_calls=[];self.camera_native_calls=[];self.camera_refuse=False
        real=b.load_module
        owner=self
        class Camera:
            """Synthetic public settings API; real helper/receipt code stays in use."""
            def __init__(inner,pin):
                raw,_=s.read_public_reference(Path(pin['path']),pin['bytes'])
                s.require(len(raw)==pin['bytes'] and s.sha(raw)==pin['sha256'],'fixture native pin changed')
                prefs=s.parse(s.read(owner.fsettings)) if owner.fsettings.exists() else {}
                inner.value=prefs.get('camera',{}).get('forceSceneAlphaBlending',False)
            def __enter__(inner):return inner
            def __exit__(inner,*unused):pass
            def read(inner):return inner.value
            def write(inner,value):inner.change(value)
            def remove_override(inner):inner.change(None)
            def change(inner,value):
                owner.camera_native_calls.append(value)
                if owner.camera_refuse:return
                inner.value=False if value is None else value
                before=s.read(owner.fsettings) if owner.fsettings.exists() else None
                prefs=s.parse(before) if before else {}
                if value is None:
                    prefs.get('camera',{}).pop('forceSceneAlphaBlending',None)
                    if not prefs.get('camera'):prefs.pop('camera',None)
                else:prefs.setdefault('camera',{})['forceSceneAlphaBlending']=value
                mode=owner.fsettings.stat().st_mode&0o777 if owner.fsettings.exists() else 0o600
                s.put(owner.fsettings,s.encoded(prefs),before);os.chmod(owner.fsettings,mode)
        self.camera_factory=Camera
        def camera_module(raw,name):
            module=real(raw,name)
            if name.startswith('sfnp_camera_'):
                restore=module.restore_owned;override=module.apply_owned_override
                def run_restore(root):
                    owner.camera_calls.append(('restore',str(root)))
                    return restore(root,api_factory=Camera,persist_timeout=0)
                def run_override(root,authority):
                    owner.camera_calls.append(('override',dict(authority)))
                    return override(root,authority,api_factory=Camera,persist_timeout=0)
                module.restore_owned=run_restore;module.apply_owned_override=run_override
            return module
        patch=mock.patch.object(b,'load_module',side_effect=camera_module);patch.start();self.addCleanup(patch.stop)

    def separate_native_settings(self):
        old=self.fsettings
        self.fsettings=self.fhome/'.config/openvr/config/steamvr.vrsettings'
        s.put(self.fsettings,s.encoded({'camera':{'enableConstructRoomView':True},'user':{'quality':3}}))
        s.put(self.fregistry,s.encoded({'config':[str(self.fsettings.parent),str(old.parent)]}),s.read(self.fregistry))
        return old

    def test_fresh_install_uses_active_first_native_config_not_historical_steam_path(self):
        old=self.separate_native_settings();active=s.read(self.fsettings);inactive=s.read(old)
        with self.fake_generation():
            b.remote_request(s,b.request(self.bundle,'bridge-prepare',pairing={**self.pair,'role':'receiver'},
                address='192.168.1.4',port=47631,automated=True),unit=self.unit)
        state=self.unit.tx.state()
        self.assertEqual(state['settings']['path'],str(self.fsettings))
        self.assertEqual(s.parse(s.read(self.froot/b.CAMERA_CONFIG))['settings'],str(self.fsettings))
        self.assertEqual(s.read(self.fsettings),active);self.assertEqual(s.read(old),inactive)
        self.assertEqual(self.camera_native_calls,[])

    def test_recovery_of_025_wrong_inactive_config_does_not_touch_real_native_file(self):
        # Reproduce the real 0.2.5 journal using the old unused target, then
        # expose the real public registry and an independently writing API.
        raw=s.read(self.fsettings)
        prefs=s.parse(raw);prefs['camera']['forceSceneAlphaBlending']=True
        s.put(self.fsettings,s.encoded(prefs),raw)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit()
        old=self.separate_native_settings();before=s.read(self.fsettings)
        # Previous global-to-session migration had a pending, explicitly owned
        # grant on the unused file; it must not authorize active-file writes.
        config=s.parse(s.read(self.froot/b.CAMERA_CONFIG))
        config['legacy_migration']={'id':'64'*32,'section_present':True,'present':False,'value':None,'installed':True}
        path=self.froot/b.CAMERA_CONFIG;previous=s.read(path);s.put(path,s.encoded(config),previous)
        state=s.parse(s.read(self.unit.tx.path))
        for op in state['operations']:
            if op['path']==str(path):op['after']['bytes']=base64.b64encode(s.encoded(config)).decode()
        s.put(self.unit.tx.path,s.encoded(state),s.read(self.unit.tx.path))
        # Exact current receipt/config must also be admitted by the existing
        # checkpoint, as a genuine staged migration would have recorded them.
        cp=self.unit.checkpoint();value=cp.value()
        # 0.2.5 did not journal the runtime pause; retain that historical shape.
        value['operations']=[op for op in value['operations'] if op['path']!=str(self.froot/'camera-session.pause')]
        value['settings']=[cp.setting(old,'camera','forceSceneAlphaBlending',[{'present':True,'value':True},{'present':False,'value':None}])]
        for op in value['operations']:
            if op['path']==str(path):op['after'].append({**state['operations'][next(i for i,o in enumerate(state['operations']) if o['path']==str(path))]['after'],'mode':0o600})
            if op['path']==str(self.unit.tx.path):op['after'][0]={'kind':'file','bytes':base64.b64encode(s.encoded(state)).decode(),'mode':0o600}
        s.put(cp.path,s.encoded(value),s.read(cp.path))
        helper=self.unit.camera_helper();helper.migration_begin(self.froot,config)
        helper.pause(self.froot,'unused')
        b.remote_request(s,b.request(self.bundle,'bridge-restore'),unit=self.unit)
        self.assertEqual(s.read(self.fsettings),before)
        self.assertEqual(self.camera_native_calls,[])
        cp.verify_restored();cp.finish(restored=True)
        self.assertFalse(self.unit.tx.path.exists())
        with self.fake_generation():
            b.remote_request(s,b.request(self.bundle,'bridge-prepare',pairing={**self.pair,'role':'receiver'},
                address='192.168.1.4',port=47631,automated=True),unit=self.unit)
        self.assertEqual(self.unit.tx.state()['settings']['path'],str(self.fsettings))
        new=s.parse(s.read(self.froot/b.CAMERA_CONFIG))
        self.assertNotIn('legacy_migration',new)
        self.assertEqual(s.read(self.fsettings),before)
        self.assertEqual(s.parse(s.read(self.froot/'camera-session.pause'))['settings'],str(self.fsettings))
        helper.startup(self.froot,running=lambda:False)
        self.assertFalse((self.froot/'camera-session.pause').exists())

    def test_update_retargets_inactive_session_and_exact_undo_restores_pause_after_config(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit();old=self.separate_native_settings();active=s.read(self.fsettings)
        self.version_bundle(pc='0.2.5',frame='0.2.6')
        with self.fake_generation():
            b.remote_request(s,b.request(self.bundle,'bridge-prepare',pairing={**self.pair,'role':'receiver'},
                address='192.168.1.4',port=47631,automated=True),unit=self.unit)
        self.assertEqual(s.parse(s.read(self.froot/b.CAMERA_CONFIG))['settings'],str(self.fsettings))
        self.assertEqual(s.parse(s.read(self.froot/'camera-session.pause'))['settings'],str(self.fsettings))
        self.assertEqual(s.read(self.fsettings),active);self.assertEqual(self.camera_native_calls,[])
        self.unit.checkpoint().restore();self.unit.checkpoint().verify_restored()
        self.assertEqual(s.parse(s.read(self.froot/b.CAMERA_CONFIG))['settings'],str(old))
        self.assertFalse((self.froot/'camera-session.pause').exists())
        self.assertEqual(s.read(self.fsettings),active)

    def test_legacy_target_update_uses_current_camera_helper_in_a_fresh_rpc_unit(self):
        self.legacy_frame(original=None)
        old=self.separate_native_settings();active=s.read(self.fsettings)
        self.version_bundle(pc='0.2.5',frame='0.2.6')
        unit=b.FrameInstall(s,home=self.fhome,steamvr=self.fsvr,root=self.froot,config=self.fhome/'.config',
            run=self.unit.run,machine=lambda:'aarch64')
        self.assertFalse(hasattr(unit,'_camera_payload'))
        with self.fake_generation():
            result=b.remote_request(s,b.request(self.bundle,'bridge-prepare',pairing={**self.pair,'role':'receiver'},
                address='192.168.1.4',port=47631,automated=True),unit=unit)
        self.assertEqual(result['reason'],'prepared-update')
        self.assertEqual(unit.tx.state()['settings']['path'],str(self.fsettings))
        self.assertEqual(s.read(self.fsettings),active);self.assertEqual(self.camera_native_calls,[])
        self.assertNotIn('legacy_migration',s.parse(s.read(self.froot/b.CAMERA_CONFIG)))

    def test_legacy_global_target_update_undo_after_config_publication_is_inactive_only(self):
        original=self.legacy_frame(original=None)
        old=self.separate_native_settings();active=s.read(self.fsettings)
        self.version_bundle(pc='0.2.5',frame='0.2.6')
        unit=b.FrameInstall(s,home=self.fhome,steamvr=self.fsvr,root=self.froot,config=self.fhome/'.config',
            run=self.unit.run,machine=lambda:'aarch64')
        with self.fake_generation():
            b.remote_request(s,b.request(self.bundle,'bridge-prepare',pairing={**self.pair,'role':'receiver'},
                address='192.168.1.4',port=47631,automated=True),unit=unit)
        self.assertEqual(s.parse(s.read(self.froot/b.CAMERA_CONFIG))['settings'],str(self.fsettings))
        b.remote_request(s,b.request(self.bundle,'bridge-restore'),unit=unit)
        unit.checkpoint().verify_restored()
        self.assertEqual(unit.tx.state(),original)
        self.assertIs(s.parse(s.read(old))['camera']['forceSceneAlphaBlending'],True)
        self.assertEqual(s.read(self.fsettings),active);self.assertEqual(self.camera_native_calls,[])
        self.assertFalse((self.froot/'camera-session.pause').exists())

    def make_bundle(self,readiness='release'):
        pins=lambda raw:{'bytes':len(raw),'sha256':s.sha(raw)}
        elf=bytearray(80);elf[:7]=b'\x7fELF\x02\x01\x01';elf[18:20]=b'\xb7\x00'
        payloads={'pc-provider/driver.vrdrivermanifest':s.encoded({'name':b.NAME,'alwaysActivate':True}),
                  'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll':b'fixture provider, never loaded',
                  'frame/'+b.LIBRARY:bytes(elf),'frame/libsfnp_stream_launch.so':bytes(elf),
                  'frame/tools/native_fragment_stream_coverage.py':b'# fixture module\n',
                  'frame/tools/compositor_startup_health.py':b'# fixture module\n',
                  'frame/tools/stream_bridge_camera.py':(ROOT/'tools/stream_bridge_camera.py').read_bytes()}
        for name in b.PROFILES:payloads[name]=b'{}\n'
        for name in b.TOOLS:payloads.setdefault(name,b'# owned fixture source, not native execution\n')
        pc_pins={}
        for role,name in [('vrlink','drivers/vrlink/bin/win64/driver_vrlink.dll'),('vrpathreg','bin/win64/vrpathreg.exe')]:
            raw=('fixture '+role).encode();s.put(self.runtime/name,raw);pc_pins[role]={'base':'steamvr','path':name,**pins(raw)}
        frame_pins={}
        for role,name in [('vrlink','tools/vrlink/bin/linuxarm64/vrlink'),('compositor','bin/linuxarm64/vrcompositor'),('fragment','resources/native.spv'),('openvr','bin/linuxarm64/libopenvr_api.so')]:
            raw=('frame fixture '+role).encode();raw=raw.ljust(128,b'_') if role=='openvr' else raw;s.put(self.fsvr/name,raw);frame_pins[role]={'base':'steamvr','path':name,**pins(raw)}
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
        with mock.patch.object(b,'generate_shaders',return_value={'coverage.spv':b'fixture coverage'}):yield

    def version_bundle(self,pc='0.2.0',frame='0.2.0'):
        self.manifest.update(version='0.2.0')
        self.manifest['pc']['version']=pc;self.manifest['frame']['version']=frame
        s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest),s.read(self.bundle_root/'stream-bridge-bundle.json'))
        self.bundle=b.Bundle(self.bundle_root,s)
        return self.bundle

    def test_semantic_version_precedence_and_rejection(self):
        ordered=['1.0.0-alpha','1.0.0-alpha.1','1.0.0-alpha.beta','1.0.0-beta','1.0.0-beta.2','1.0.0-beta.11','1.0.0-rc.1','1.0.0','1.0.1','2.0.0']
        for a,z in zip(ordered,ordered[1:]):
            self.assertTrue(b.older_version(a,z));self.assertFalse(b.older_version(z,a))
        self.assertFalse(b.older_version('0.2.0+one','0.2.0+two'))
        self.assertFalse(b.older_version('0.2.0',None))
        for bad in ('1.2','01.2.3','1.2.3-01','1.2.3-','v1.2.3','1.2.3\n','1.2.3\x1b[31m',1,True,'1.2.3+'+'x'*81):
            with self.subTest(bad=repr(bad)),self.assertRaises(ValueError):b.patch_version(bad)
        for side in ('pc','frame'):
            self.manifest[side]['version']=None
            s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest),s.read(self.bundle_root/'stream-bridge-bundle.json'))
            with self.assertRaises(ValueError):b.Bundle(self.bundle_root,s)
            del self.manifest[side]['version']

    def test_versioned_install_records_independent_component_versions(self):
        self.version_bundle(pc='0.2.0',frame='0.2.1')
        installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit()
        self.assertEqual(installed.state()['version'],'0.2.0');self.assertEqual(self.unit.tx.state()['version'],'0.2.1')
        self.assertEqual(b.version_status(installed.state(),self.bundle.pc),{'installed':True,'installed_version':'0.2.0','target_version':'0.2.0'})
        self.assertEqual(self.unit.inventory(self.bundle)['installed_version'],'0.2.1')
        self.assertEqual(self.unit.inventory(self.bundle)['target_version'],'0.2.1')
        self.assertTrue(self.unit.inventory(self.bundle)['files_current'])

    def test_version_update_migrates_legacy_receipts_and_exact_undo_retains_missing_version(self):
        installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();pc_before=s.read(installed.receipt);frame_before=s.read(self.unit.tx.path)
        settings=s.read(self.settings);frame_settings=s.read(self.fsettings)
        credential=self.local.root/'pairings'/(self.pair['pair_id']+'.json');pair=s.read(credential,4096,private=True)
        self.assertIsNone(b.version_status(installed.state(),self.bundle.pc)['installed_version'])
        self.version_bundle()
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit()
        self.assertEqual(installed.state()['version'],'0.2.0');self.assertEqual(self.unit.tx.state()['version'],'0.2.0')
        self.assertEqual(s.read(credential,4096,private=True),pair)
        self.assertEqual(s.read(self.settings),settings);self.assertEqual(s.read(self.fsettings),frame_settings)
        self.assertFalse(any('restart' in arg for command in self.commands for arg in command))
        installed.checkpoint().restore();installed.checkpoint().verify_restored()
        self.unit.checkpoint().restore();self.unit.checkpoint().verify_restored()
        self.assertEqual(s.read(installed.receipt),pc_before);self.assertEqual(s.read(self.unit.tx.path),frame_before)
        self.assertNotIn('version',installed.state());self.assertNotIn('version',self.unit.tx.state())

    def test_version_downgrades_refuse_both_components_before_writes(self):
        self.version_bundle('0.2.0','0.2.0');installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit()
        for target in ('0.1.9',None):
            with self.subTest(target=target):
                self.version_bundle('0.1.9','0.1.9')
                if target is None:
                    for side in ('pc','frame'):del self.manifest[side]['version']
                    del self.manifest['version']
                    s.put(self.bundle_root/'stream-bridge-bundle.json',s.encoded(self.manifest),s.read(self.bundle_root/'stream-bridge-bundle.json'))
                    self.bundle=b.Bundle(self.bundle_root,s)
                before={str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}
                with self.assertRaisesRegex(s.SetupError,'newer'):installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)
                with self.assertRaisesRegex(s.SetupError,'newer'):
                    self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
                self.assertEqual(before,{str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()})

    def test_version_does_not_override_changed_file_hashes(self):
        self.version_bundle();installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        path=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'
        s.put(path,b'foreign modification',s.read(path))
        with self.assertRaisesRegex(s.SetupError,'Changed provider'):
            installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631,backup=True)

    def test_version_same_number_changed_bundle_still_offers_update_and_logs_both_roles(self):
        import contextlib,types
        self.version_bundle();installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit()
        name='pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'
        raw=s.read(self.bundle_root/name)+b' changed package, same semantic version'
        s.put(self.bundle_root/name,raw,s.read(self.bundle_root/name))
        self.manifest['files'][name]={'bytes':len(raw),'sha256':s.sha(raw)};self.version_bundle()
        choices=[];plans=[];calls=[]
        class Screen:
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,*a):pass
            def inventory(inner,**kw):pass
            def version_plan(inner,**kw):plans.append(kw)
            def choose_action(inner,actions,**kw):choices.append((actions,kw));return 'update'
            def confirm(inner,*a):return False
            def suspend(inner):return contextlib.nullcontext()
        class Peer:
            def call(inner,request):calls.append(request['command']);return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:Screen())):
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        self.assertIn('update',choices[0][0]);self.assertEqual(choices[0][1]['recommended'],'update')
        self.assertEqual(plans[0]['pc'],{'installed':True,'installed_version':'0.2.0','target_version':'0.2.0'})
        self.assertEqual(plans[0]['frame'],plans[0]['pc'])
        self.assertEqual(calls,['bridge-inventory'])
        events=[json.loads(line) for path in (self.local.root/'logs').glob('*.jsonl') for line in path.read_text().splitlines()]
        versions=[e['versions'] for e in events if 'versions'in e]
        self.assertEqual(versions,[plans[0]])

    def test_runtime_versions_reach_real_pc_and_remote_inspection_without_granting_compatibility(self):
        import contextlib,io,types
        s.put(self.runtime/'version.vrmanifest',s.encoded({'version':'2.18.2'}))
        s.put(self.fsvr/'version.vrmanifest',s.encoded({'major':2,'minor':18,'build':3}))
        s.put(self.fsvr/'tools/vrlink/version.vrmanifest',s.encoded({'version':'4.5.6'}))
        # A plausible version label cannot repair a changed pinned runtime file.
        path=self.fsvr/'bin/linuxarm64/vrcompositor';path.write_bytes(b'changed native file')
        class Peer:
            def call(inner,request):return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        args=types.SimpleNamespace(bundle=self.bundle_root,check=True,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        output=io.StringIO()
        before={str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}
        with contextlib.redirect_stdout(output):self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        report=json.loads(output.getvalue())
        self.assertEqual(report['pc']['runtime_versions']['steamvr']['version'],'2.18.2')
        self.assertEqual(report['frame']['runtime_versions']['steamvr']['version'],'2.18.3')
        self.assertEqual(report['frame']['runtime_versions']['steam_link']['version'],'4.5.6')
        self.assertFalse(report['frame']['runtime_compatible'])
        self.assertEqual(before,{str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()})

    def test_version_inspection_reports_missing_and_unversioned_independently(self):
        import contextlib,io,types
        installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        self.version_bundle('0.2.0','0.2.1')
        class Peer:
            def call(inner,request):return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        args=types.SimpleNamespace(bundle=self.bundle_root,check=True,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        output=io.StringIO()
        before={str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()}
        with contextlib.redirect_stdout(output):self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        report=json.loads(output.getvalue())
        self.assertTrue(report['pc']['installed']);self.assertIsNone(report['pc']['installed_version'])
        self.assertFalse(report['frame']['installed']);self.assertIsNone(report['frame']['installed_version'])
        self.assertEqual(report['pc']['target_version'],'0.2.0');self.assertEqual(report['frame']['target_version'],'0.2.1')
        self.assertEqual(report['target_versions'],{'pc':'0.2.0','frame':'0.2.1'})
        self.assertEqual(before,{str(p):p.read_bytes() for p in self.base.rglob('*') if p.is_file()})

    def test_version_newer_frame_does_not_offer_an_update_or_repair(self):
        import contextlib,types
        self.version_bundle('0.2.0','0.3.0');installed=b.LocalInstall(s,self.local)
        installed.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631)
        self.unit.tx.commit();self.version_bundle();choices=[];messages=[];calls=[]
        class Screen:
            def banner(inner,**kw):pass
            def status(inner,*a):pass
            def path(inner,*a,**kw):pass
            def say(inner,key):messages.append(key)
            def inventory(inner,**kw):pass
            def choose_action(inner,actions,**kw):choices.append((actions,kw));return 'exit'
            def suspend(inner):return contextlib.nullcontext()
        class Peer:
            def call(inner,request):calls.append(request['command']);return b.remote_request(s,request,unit=self.unit)
            def close(inner):pass
        args=types.SimpleNamespace(bundle=self.bundle_root,check=False,dry_run=False,lang='en',frame='192.168.1.25',user='steamos',action=None)
        with mock.patch.object(s.sys.stdin,'isatty',return_value=True),mock.patch.object(s,'load_presentation',return_value=types.SimpleNamespace(Presenter=lambda **kw:Screen())):
            self.assertEqual(b.run(args,s,self.local,remote_factory=lambda *a,**kw:Peer()),0)
        self.assertFalse({'install','update','repair'}&set(choices[0][0]))
        self.assertEqual(choices[0][1]['recommended'],'check');self.assertIn('version_downgrade',messages)
        self.assertEqual(calls,['bridge-inventory'])

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

    def legacy_frame(self,original=False):
        # Seed a genuine old receipt shape and its owned global True without
        # executing old code. Every predecessor/payload remains exact and private.
        prefs=s.parse(s.read(self.fsettings))
        if original is None:prefs['camera'].pop('forceSceneAlphaBlending',None)
        else:prefs['camera']['forceSceneAlphaBlending']=original
        s.put(self.fsettings,s.encoded(prefs),s.read(self.fsettings))
        self.version_bundle(pc='0.2.4',frame='0.2.4')
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit();state=self.unit.tx.state();state['settings'].pop('policy')
        remaining=[]
        for op in state['operations']:
            if op['path'] in (str(self.froot/b.CAMERA_CONFIG),str(self.froot/b.CAMERA_TOOL),
                str(self.fhome/'.config/systemd/user/steamvr.service.d'/b.CAMERA_HOOK)):
                self.unit.tx.replace(op,reverse=True)
            else:remaining.append(op)
        state['operations']=remaining
        s.put(self.unit.tx.path,s.encoded(state),s.read(self.unit.tx.path))
        prefs=s.parse(s.read(self.fsettings));prefs.setdefault('camera',{})['forceSceneAlphaBlending']=True
        s.put(self.fsettings,s.encoded(prefs),s.read(self.fsettings))
        return state

    def update_frame_version(self,version):
        self.version_bundle(pc='0.2.4',frame=version)
        with self.fake_generation():
            result=self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.unit.tx.commit()
        return result

    def test_legacy_update_restores_owned_true_via_real_helper_before_restart(self):
        before=self.legacy_frame()
        result=self.update_frame_version('0.2.5')
        self.assertEqual(result['reason'],'prepared-update')
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],False)
        self.assertEqual(self.camera_native_calls,[False])
        state=self.unit.tx.state()
        self.assertEqual(state['settings'],{**before['settings'],'policy':b.SESSION_POLICY})
        config=s.parse(s.read(self.froot/b.CAMERA_CONFIG,private=True))
        migration=config['legacy_migration']
        self.assertRegex(migration['id'],'^[0-9a-f]{64}$')
        self.assertEqual({k:v for k,v in migration.items() if k!='id'},
            {'section_present':True,'present':True,'value':False,'installed':True})
        marker=s.parse(s.read(self.froot/'camera-session-migration.json',private=True))
        self.assertEqual(marker['id'],migration['id']);self.assertEqual(marker['phase'],'consumed')
        self.assertFalse(any('camera-session-migration.json' in op['path'] for op in state['operations']))
        self.assertTrue(self.unit.inventory(self.bundle)['files_current'])
        self.assertFalse(any('restart' in cmd or 'stop' in cmd for cmd in self.commands))

    def test_legacy_update_checkpoint_undo_restores_native_true_and_reupgrade_has_new_grant(self):
        original=self.legacy_frame();self.update_frame_version('0.2.5')
        first=s.parse(s.read(self.froot/b.CAMERA_CONFIG,private=True))['legacy_migration']['id']
        self.unit.checkpoint().restore()
        self.assertEqual(self.unit.tx.state(),original)
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],True)
        self.assertEqual(self.camera_native_calls,[False,True])
        self.assertFalse((self.froot/b.CAMERA_CONFIG).exists())
        self.assertFalse((self.froot/b.CAMERA_TOOL).exists())
        self.unit.checkpoint().verify_restored();self.unit.checkpoint().finish(restored=True)
        self.update_frame_version('0.2.5')
        second=s.parse(s.read(self.froot/b.CAMERA_CONFIG,private=True))['legacy_migration']['id']
        self.assertNotEqual(first,second)
        self.assertEqual(self.camera_native_calls,[False,True,False])
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],False)

    def test_session_update_keeps_grant_and_preserves_user_changed_true_through_uninstall(self):
        self.legacy_frame();self.update_frame_version('0.2.5')
        config=s.parse(s.read(self.froot/b.CAMERA_CONFIG,private=True));self.unit.checkpoint().finish()
        prefs=s.parse(s.read(self.fsettings));prefs['camera']['forceSceneAlphaBlending']=True;prefs['user']['later']=17
        s.put(self.fsettings,s.encoded(prefs),s.read(self.fsettings));before=s.read(self.fsettings)
        self.update_frame_version('0.2.6')
        self.assertEqual(s.parse(s.read(self.froot/b.CAMERA_CONFIG))['legacy_migration'],config['legacy_migration'])
        self.assertEqual(s.read(self.fsettings),before)
        self.unit.checkpoint().finish()
        self.unit.undo(live=True)
        self.assertEqual(s.read(self.fsettings),before)
        self.assertEqual(self.camera_native_calls,[False])

    def test_legacy_uninstall_and_undo_use_exact_transmitted_helper_and_native_before(self):
        original=self.legacy_frame()
        self.version_bundle(pc='0.2.4',frame='0.2.5')
        result=b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertEqual(result['reason'],'uninstalled')
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],False)
        self.assertIsNone(self.unit.tx.state())
        self.assertEqual(self.camera_native_calls,[False])
        b.remote_request(s,b.request(self.bundle,'bridge-restore'),unit=self.unit)
        self.assertEqual(self.unit.tx.state(),original)
        self.assertEqual(self.camera_native_calls,[False,True])
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],True)
        self.unit.checkpoint().verify_restored()

    def test_cold_generation_failure_preserves_active_camera_lease_without_pausing(self):
        self.version_bundle(pc='0.2.4',frame='0.2.5')
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit()
        with self.unit.camera_context() as (module,root):
            config,sha=module.load_config(root)
            session=module.Session(root,config,sha,self.camera_factory(config['library']));session.enable()
        self.addCleanup(session.restore)
        before=s.read(self.unit.tx.path);lease=s.read(self.froot/'camera-session-lease.json',private=True)
        calls=list(self.camera_calls);native=list(self.camera_native_calls)
        self.version_bundle(pc='0.2.4',frame='0.2.6')
        for boundary in ('generation','backup'):
            with self.subTest(boundary=boundary):
                failure=mock.patch.object(b,'generate_shaders',side_effect=OSError('owned preparation failure')) if boundary=='generation' else mock.patch.object(self.unit,'backup',side_effect=OSError('owned preparation failure'))
                with self.fake_generation(),failure:
                    with self.assertRaisesRegex(OSError,'owned preparation failure'):
                        self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
                self.assertEqual(s.read(self.unit.tx.path),before)
                self.assertEqual(s.read(self.froot/'camera-session-lease.json',private=True),lease)
                self.assertEqual(self.camera_calls,calls);self.assertEqual(self.camera_native_calls,native)
                self.assertFalse((self.froot/'camera-session.pause').exists())
                self.assertFalse(self.unit.checkpoint().path.exists())
                self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],True)

    def active_camera(self):
        with self.unit.camera_context() as (module,root):
            config,sha=module.load_config(root)
            session=module.Session(root,config,sha,self.camera_factory(config['library']));session.enable()
        return session

    def test_session_update_quiesces_real_live_lease_before_replacing_configuration(self):
        self.version_bundle(pc='0.2.4',frame='0.2.5')
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit();session=self.active_camera()
        self.assertTrue((self.froot/'camera-session-lease.json').exists())
        self.update_frame_version('0.2.6')
        self.assertEqual(self.camera_native_calls,[True,False])
        self.assertFalse((self.froot/'camera-session-lease.json').exists())
        self.assertTrue((self.froot/'camera-session.pause').exists())
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],False)
        with self.assertRaisesRegex(RuntimeError,'paused'):session.enable()
        self.assertTrue(self.unit.inventory(self.bundle)['files_current'])

    def test_session_uninstall_restores_real_live_lease_before_deleting_helper(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit();self.active_camera()
        self.unit.undo(live=True)
        self.assertEqual(self.camera_native_calls,[True,False])
        self.assertFalse((self.froot/'camera-session-lease.json').exists())
        self.assertFalse((self.froot/b.CAMERA_TOOL).exists())
        self.assertFalse((self.froot/b.CAMERA_CONFIG).exists())
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],False)

    def test_checkpoint_foreign_payload_refuses_before_any_native_camera_override(self):
        self.legacy_frame();self.update_frame_version('0.2.5')
        target=self.froot/b.CAMERA_TOOL;before=s.read(target);s.put(target,before+b' foreign modification',before)
        calls=list(self.camera_calls);native=list(self.camera_native_calls)
        with self.assertRaisesRegex(s.SetupError,'changed file was preserved'):
            self.unit.checkpoint().restore()
        self.assertEqual(self.camera_calls,calls);self.assertEqual(self.camera_native_calls,native)
        self.assertTrue((self.froot/b.CAMERA_TOOL).exists());self.assertTrue((self.froot/b.CAMERA_CONFIG).exists())
        self.assertEqual(s.read(target),before+b' foreign modification')

    def test_checkpoint_foreign_camera_value_refuses_before_native_override(self):
        self.legacy_frame();self.update_frame_version('0.2.5')
        prefs=s.parse(s.read(self.fsettings));prefs['camera']['forceSceneAlphaBlending']='foreign-value'
        s.put(self.fsettings,s.encoded(prefs),s.read(self.fsettings))
        calls=list(self.camera_calls);native=list(self.camera_native_calls)
        with self.assertRaisesRegex(s.SetupError,'Changed camera checkpoint preserved'):
            self.unit.checkpoint().restore()
        self.assertEqual(self.camera_calls,calls);self.assertEqual(self.camera_native_calls,native)
        self.assertEqual(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],'foreign-value')

    def test_legacy_absent_key_is_removed_only_through_native_api(self):
        self.legacy_frame(original=None);self.update_frame_version('0.2.5')
        prefs=s.parse(s.read(self.fsettings))
        self.assertNotIn('forceSceneAlphaBlending',prefs.get('camera',{}))
        self.assertEqual(prefs['user'],{'quality':3})
        self.assertEqual(self.camera_native_calls,[None])
        grant=s.parse(s.read(self.froot/b.CAMERA_CONFIG))['legacy_migration']
        self.assertFalse(grant['present']);self.assertIsNone(grant['value'])

    def test_legacy_helper_hash_tamper_refuses_before_native_change_or_payload_removal(self):
        self.legacy_frame();self.version_bundle(pc='0.2.4',frame='0.2.5')
        before=s.read(self.unit.tx.path);settings=s.read(self.fsettings)
        request=b.request(self.bundle,'bridge-undo',automated=True)
        request['camera_source']=base64.b64encode(b'foreign camera helper').decode()
        with self.assertRaisesRegex(s.SetupError,'differs from the checked package'):
            b.remote_request(s,request,unit=self.unit)
        self.assertEqual(s.read(self.unit.tx.path),before);self.assertEqual(s.read(self.fsettings),settings)
        self.assertEqual(self.camera_calls,[]);self.assertEqual(self.camera_native_calls,[])

    def test_live_camera_refusal_keeps_static_files_for_recovery(self):
        self.legacy_frame();self.version_bundle(pc='0.2.4',frame='0.2.5');self.camera_refuse=True
        with self.fake_generation(),self.assertRaisesRegex(s.SetupError,'legacy runtime restore not confirmed'):
            self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,backup=True,live=True)
        self.assertTrue((self.froot/b.CAMERA_CONFIG).exists())
        self.assertTrue((self.froot/b.CAMERA_TOOL).exists())
        self.assertTrue(self.unit.checkpoint().path.exists())
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],True)

    def test_fresh_frame_install_preserves_camera_setting_and_stages_session_startup(self):
        before=s.read(self.fsettings)
        with self.fake_generation():
            self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.assertEqual(s.read(self.fsettings),before,'Installation must not globally enable native camera blending')
        self.unit.tx.commit();state=self.unit.tx.state()
        self.assertEqual(state['settings']['policy'],'session-managed-v1')
        config=s.parse(s.read(self.froot/'camera-session.json',private=True))
        pin=self.bundle.frame['runtime_files']['openvr']
        self.assertEqual(config,{'schema':'sfnp-camera-session-config-v1','settings':str(self.fsettings),
            'library':{'path':str(self.fsvr/pin['path']),'bytes':pin['bytes'],'sha256':pin['sha256']}})
        hook=self.fhome/'.config/systemd/user/steamvr.service.d/85-steam-link-native-passthrough-camera.conf'
        text=hook.read_text()
        self.assertIn('ExecCondition=-/usr/bin/python3 -I -B ',text)
        self.assertNotIn('ExecStartPre=',text,'Valve starts vrstartup in its own pre-start list; append would run too late')
        self.assertIn('stream_bridge_camera.py',text);self.assertIn(' startup --root ',text)
        self.assertNotIn('Environment=',text);self.assertNotIn('ExecStop',text)
        self.assertEqual((self.froot/'camera-session.json').stat().st_mode&0o777,0o600)
        self.assertIn('frame/tools/stream_bridge_camera.py',b.TOOLS)
        self.assertFalse(any(op['path'].endswith(('camera-session-lease.json','camera-session-migration.json')) for op in state['operations']))
        self.assertTrue(self.unit.inventory(self.bundle)['files_current'])

    def test_fresh_frame_install_does_not_create_a_missing_camera_settings_file(self):
        self.fsettings.unlink()
        with self.fake_generation():
            self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.assertFalse(self.fsettings.exists(),'A session-managed install must not create global camera settings')
        self.unit.tx.commit()
        entry=self.unit.tx.state()['settings']['baseline']['camera']['keys']['forceSceneAlphaBlending']
        self.assertFalse(entry['present']);self.assertIsNone(entry['value'])
        self.assertEqual(self.unit.undo(live=True)['reason'],'uninstalled')
        self.assertFalse(self.fsettings.exists())

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

    def old_frame_only(self):
        data=self.fhome/'.local/share/SteamFrameNativePassthrough'
        config=self.fhome/'.config/SteamFrameNativePassthrough'
        dropin=self.fhome/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf'
        link=self.fhome/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json'
        payload=data/'package/library.so';s.put(payload,b'owned retired fixture; never loaded')
        s.put(dropin,b'[Service]\nExecCondition=/owned/retired-activation\n')
        s.put(config/'control.json',s.encoded({'schema':'sfnp-user-control-v1','enabled':True}))
        target='/run/user/'+str(os.getuid())+'/steam-frame-native-passthrough/receiver.json'
        link.parent.mkdir(parents=True,mode=0o700,exist_ok=True);os.symlink(target,link)
        s.put(config/'install.json',s.encoded({'schema':'sfnp-user-install-v1','home':str(self.fhome),'data':str(data),
            'manifest_link':target,'package_sha256':'a'*64,'files':{str(payload):s.sha(s.read(payload)),str(dropin):s.sha(s.read(dropin))}}))
        s.put(config/'camera-before.json',s.encoded({'schema':'sfnp-camera-key-v1','settings':str(self.fsettings),
            'previous':{'present':False,'value':None},'write_intent':True}))
        before=s.read(self.fsettings);prefs=s.parse(before);prefs['camera']['forceSceneAlphaBlending']=True
        s.put(self.fsettings,s.encoded(prefs),before)
        return data,config,dropin,link,payload

    def test_explicit_uninstall_removes_both_generations_without_reactivating_retired_layer(self):
        data,config,dropin,link,payload=self.old_frame_only()
        foreign=data/'untracked-user-file';s.put(foreign,b'keep')
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit();installed=s.read(self.unit.tx.path)
        self.assertFalse(dropin.exists());self.assertFalse(os.path.lexists(link))
        replace=self.unit.tx.replace
        def never_reactivate(operation,reverse=False):
            if reverse and operation['path'] in (str(dropin),str(link),str(config/'control.json')):
                self.fail('Explicit Uninstall must never restore the retired activation, even briefly')
            return replace(operation,reverse=reverse)
        with mock.patch.object(self.unit.tx,'replace',side_effect=never_reactivate):
            result=b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertEqual(result['reason'],'uninstalled')
        for path in (dropin,link,payload,config/'install.json',config/'control.json',config/'camera-before.json'):
            self.assertFalse(os.path.lexists(path),str(path))
        self.assertFalse((self.froot/b.LIBRARY).exists());self.assertEqual(s.read(foreign),b'keep')
        self.assertNotIn('forceSceneAlphaBlending',s.parse(s.read(self.fsettings)).get('camera',{}))
        self.assertEqual(self.camera_native_calls[-1],None)
        # Explicit Undo of a failed Uninstall restores the exact pre-Uninstall
        # paired state. It does not turn the old inactive layer back on.
        b.remote_request(s,b.request(self.bundle,'bridge-restore'),unit=self.unit)
        self.unit.checkpoint().verify_restored()
        self.assertEqual(s.read(self.unit.tx.path),installed)
        self.assertFalse(dropin.exists());self.assertFalse(os.path.lexists(link))
        self.assertFalse(s.parse(s.read(config/'control.json'))['enabled'])

    def test_legacy_only_uninstall_and_undo_use_pinned_native_camera_api(self):
        data,config,dropin,link,payload=self.old_frame_only()
        original={path:s.read(path) for path in (payload,dropin,config/'install.json',config/'control.json',config/'camera-before.json')}
        result=b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertEqual(result['reason'],'uninstalled');self.assertTrue(self.unit.checkpoint().path.exists())
        self.assertFalse(dropin.exists());self.assertFalse(os.path.lexists(link))
        self.assertNotIn('forceSceneAlphaBlending',s.parse(s.read(self.fsettings)).get('camera',{}))
        b.remote_request(s,b.request(self.bundle,'bridge-restore'),unit=self.unit)
        self.unit.checkpoint().verify_restored()
        self.assertIs(s.parse(s.read(self.fsettings))['camera']['forceSceneAlphaBlending'],True)
        self.assertEqual(self.camera_native_calls,[None,True])
        self.assertTrue(os.path.islink(link))
        for path,raw in original.items():self.assertEqual(s.read(path),raw)

    def test_partial_uninstall_with_missing_camera_tools_finishes_without_reactivating_patch(self):
        with self.fake_generation():self.unit.install(self.bundle,{**self.pair,'role':'receiver'},'192.168.1.4',47631,live=True)
        self.unit.tx.commit();initial=s.read(self.unit.tx.path);replace=self.unit.tx.replace
        def interrupt(operation,reverse=False):
            if operation['path']==str(self.froot/b.LIBRARY):raise OSError('uncertain interrupted removal')
            return replace(operation,reverse=reverse)
        with mock.patch.object(self.unit.tx,'replace',side_effect=interrupt),self.assertRaisesRegex(OSError,'interrupted'):
            b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertFalse((self.froot/b.CAMERA_CONFIG).exists());self.assertFalse((self.froot/b.CAMERA_TOOL).exists())
        self.assertTrue((self.froot/b.LIBRARY).exists());self.assertEqual(s.read(self.unit.tx.path),initial)
        checkpoint=self.unit.checkpoint();checkpoint.retain_current(self.bundle.pin)
        def never_activate(operation,reverse=False):
            if operation['path']!=str(self.froot/b.CAMERA_CONFIG):self.assertIsNone(operation['after'])
            return replace(operation,reverse=reverse)
        with mock.patch.object(self.unit.tx,'replace',side_effect=never_activate):
            result=b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertEqual(result['reason'],'uninstalled');self.assertFalse(self.unit.tx.path.exists())
        self.assertFalse((self.froot/b.LIBRARY).exists());self.assertFalse((self.froot/b.CAMERA_CONFIG).exists())
        checkpoint.restore();checkpoint.verify_restored()
        self.assertEqual(s.read(self.unit.tx.path),initial)

    def test_explicit_pc_uninstall_unregisters_owned_path_even_when_preexisting(self):
        self.local.register(self.runtime/'bin/win64/vrpathreg.exe','adddriver')
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        self.assertTrue(installer.state()['registered_before'])
        result=installer.uninstall(self.local.inventory(),backup=True)
        self.assertEqual(result['reason'],'uninstalled');self.assertFalse(self.local.inventory()['registered'])
        self.assertTrue(installer.checkpoint().value()['registered'],'Explicit Undo retains the original registration authority')

    def test_interrupted_retired_uninstall_keeps_receipt_last_and_continues_from_exact_checkpoint(self):
        _,config,dropin,link,payload=self.old_frame_only();replace=self.unit.tx.replace
        def interrupt(operation,reverse=False):
            if operation['path']==str(config/'control.json'):raise OSError('interrupted retired removal')
            if operation['path']==str(config/'install.json'):
                self.assertFalse(payload.exists());self.assertFalse(dropin.exists());self.assertFalse(os.path.lexists(link))
                self.assertFalse((config/'camera-before.json').exists());self.assertFalse((config/'control.json').exists())
            return replace(operation,reverse=reverse)
        with mock.patch.object(self.unit.tx,'replace',side_effect=interrupt),self.assertRaisesRegex(OSError,'interrupted retired'):
            b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertTrue((config/'install.json').exists());self.assertFalse(payload.exists());self.assertFalse(dropin.exists())
        # Older releases could remove the receipts too early. Their exact
        # checkpoint is sufficient in memory; neither file is reinstalled.
        (config/'install.json').unlink();(config/'camera-before.json').unlink()
        self.unit.checkpoint().retain_current(self.bundle.pin)
        def complete(operation,reverse=False):
            self.assertFalse(reverse);self.assertIsNone(operation['after'])
            if operation['path']==str(config/'install.json'):
                self.assertFalse((config/'control.json').exists())
            return replace(operation,reverse=reverse)
        with mock.patch.object(self.unit.tx,'replace',side_effect=complete):
            result=b.remote_request(s,b.request(self.bundle,'bridge-undo',automated=True),unit=self.unit)
        self.assertEqual(result['reason'],'uninstalled')
        for path in (payload,dropin,link,config/'control.json',config/'install.json',config/'camera-before.json'):
            self.assertFalse(os.path.lexists(path))
        self.assertNotIn('forceSceneAlphaBlending',s.parse(s.read(self.fsettings)).get('camera',{}))

    def test_pc_uninstall_tolerates_missing_owned_payload_but_preserves_changed_remaining_file(self):
        installer=b.LocalInstall(s,self.local)
        installer.install(self.bundle,self.local.inventory(),self.pair,'192.168.1.4',47631)
        image=self.local.root/'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'
        image.unlink()
        other=next(self.local.root/name for name in sorted(b.PC_FILES) if self.local.root/name!=image)
        before=s.read(other);s.put(other,b'foreign replacement',before)
        with self.assertRaisesRegex(s.SetupError,'Changed provider'):installer.uninstall(self.local.inventory(),backup=True)
        self.assertTrue(installer.receipt.exists());self.assertTrue(self.local.inventory()['registered'])
        self.assertEqual(s.read(other),b'foreign replacement');self.assertFalse(installer.checkpoint().path.exists())
        s.put(other,before,s.read(other));installer.uninstall(self.local.inventory(),backup=True)
        self.assertFalse(installer.receipt.exists());self.assertFalse(self.local.inventory()['registered'])



    def test_generation_is_coverage_only_and_ignores_cached_tools(self):
        import types,sys
        coverage=b'owned fixture coverage'
        files={name:b'# unused own fixture source\n' for name in b.TOOLS}
        files['frame/tools/native_fragment_ssa_probe.py']=b'IDENTITY="owned"\n'
        files['frame/tools/native_fragment_stream_coverage.py']=b'import native_fragment_ssa_probe as s\ndef transform(original):\n assert s.IDENTITY=="owned"\n return b"owned fixture coverage",{}\n'
        for name in b.PROFILES:files[name]=b'owned profile'
        bundle=types.SimpleNamespace(frame={'files':sorted(files)},files=files)
        pins={name:{'bytes':len(raw),'sha256':s.sha(raw)} for name,raw in [('coverage.spv',coverage)]}
        cached=types.ModuleType('native_fragment_ssa_probe');cached.IDENTITY='foreign';cached.__file__='/foreign/cached.py'
        previous=sys.modules.get('native_fragment_ssa_probe');sys.modules['native_fragment_ssa_probe']=cached
        try:
            with tempfile.TemporaryDirectory(prefix='sfnp-own-source-') as stage,mock.patch.object(b,'GENERATED',pins):
                result=b.generate_shaders(s,bundle,b'owned fixture input',Path(stage))
            self.assertEqual(result,{'coverage.spv':coverage});self.assertIs(sys.modules['native_fragment_ssa_probe'],cached)
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
        env={'HOME':str(self.fhome),'XDG_DATA_HOME':'','XDG_CONFIG_HOME':'','VR_CONFIG_PATH':'','VR_PATHREG_OVERRIDE':''}
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
