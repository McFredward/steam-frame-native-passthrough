"""Exact-package full-resolution connection setup, with no automatic restart.

This module is loaded by the verified setup entry. Prototype manifests provide
inventory only. Source identities and successful software tests do not turn a
prototype into an installable release.
"""
from __future__ import annotations
import base64
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import secrets
import shlex
import shutil
import socket
import stat
import sys
import tempfile

SCHEMA = 'sfnp-stream-bridge-bundle-v1'
RECEIPT = 'sfnp-stream-bridge-install-v1'
MARKER = b'SFNP_STREAM_RECEIVER/1\n'
NAME = 'steam_link_native_passthrough'
LAYER = 'VK_LAYER_SFNP_stream_bridge'
LIBRARY = 'libVkLayer_steam_link_bridge.so'
MANIFEST = 'VkLayer_SFNP_stream_bridge.json'
SETTINGS_SECTION = 'driver_steam_link_native_passthrough'
PC_FILES = {'pc-provider/driver.vrdrivermanifest',
            'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'}
TOOLS = frozenset('frame/tools/'+name+'.py' for name in (
    'app_payload_probe','compositor_startup_health','native_fragment_atlas_probe',
    'native_fragment_packed_output','native_fragment_r1_presenter','native_fragment_scene_presenter',
    'native_fragment_ssa_probe','native_fragment_stream_coverage','pixel_payload_recovery'))
PROFILES = frozenset('frame/docs/protocols/scene-contribution-atlas-v'+str(n)+'.json' for n in (1,3))
GENERATED = {'coverage.spv': {'bytes':7140,'sha256':'54760d17ec2933ca667923ca713f64d6810914f9b2b53503d9a6ebb68ee2bdc0'},
             'legacy.spv': {'bytes':31384,'sha256':'44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49'}}


class Bundle:
    def __init__(self, root, engine):
        self.engine = engine
        self.root = engine.no_links(root)
        raw = engine.read(self.root / 'stream-bridge-bundle.json',128*1024)
        self.value = engine.parse(raw)
        self.pin = engine.sha(engine.encoded(self.value))
        v = self.value
        engine.require(set(v)=={'schema','readiness','bridge_protocol','pc','frame','files'} and
                       v['schema']==SCHEMA and v['readiness'] in ('prototype','release'),
                       'Unknown full-resolution setup package.')
        self.protocol = engine.protocol(v['bridge_protocol'])
        engine.records(v['files'])
        self.files = {}
        for name,pin in v['files'].items():
            raw = engine.read(self.root/engine.relative(name),pin['bytes'])
            engine.require(len(raw)==pin['bytes'] and engine.sha(raw)==pin['sha256'],
                           'The setup package is incomplete or changed; existing files were preserved.')
            self.files[name] = raw
        engine.require(sum(map(len,self.files.values()))<=engine.MAX_TOTAL,'Package exceeds setup size limit.')
        self.pc, self.frame = v['pc'], v['frame']
        engine.require(type(self.pc)is dict and set(self.pc)=={'reviewed','platform','files','runtime_files'} and
                       self.pc['platform']=='windows-x86_64' and type(self.pc['reviewed'])is bool and
                       type(self.pc['files'])is list and set(self.pc['files'])==PC_FILES,
                       'Unsupported PC connection package.')
        engine.require(type(self.frame)is dict and set(self.frame)=={'reviewed','files','runtime_files','generated'} and
                       type(self.frame['reviewed'])is bool and type(self.frame['files'])is list and
                       len(self.frame['files'])==len(set(self.frame['files'])) and
                       set(self.frame['files'])==TOOLS|PROFILES|{'frame/'+LIBRARY,'frame/libsfnp_stream_launch.so'} and
                       self.frame['generated']==GENERATED and
                       {'frame/'+LIBRARY,'frame/tools/native_fragment_stream_coverage.py',
                        'frame/tools/native_fragment_scene_presenter.py','frame/libsfnp_stream_launch.so',
                        'frame/tools/compositor_startup_health.py'}<=set(self.frame['files']) and
                       all(n in PROFILES|{'frame/'+LIBRARY,'frame/libsfnp_stream_launch.so'} or re.fullmatch(r'frame/tools/[a-z0-9_]+[.]py',n)
                           for n in self.frame['files']), 'Unsupported Frame connection package.')
        engine.require(set(self.files)==set(self.pc['files'])|set(self.frame['files']),
                       'Unknown executable or shader payload in package.')
        provider=engine.parse(self.files['pc-provider/driver.vrdrivermanifest'])
        engine.require(provider.get('name')==NAME and provider.get('alwaysActivate') is True,
                       'Unsupported owned OpenVR provider manifest.')
        for side in (self.pc,self.frame):
            pins=side['runtime_files']
            engine.require(type(pins)is dict and 1<=len(pins)<=32,'Complete runtime file pins are required.')
            for role,pin in pins.items():
                engine.require(type(role)is str and re.fullmatch('[a-z_]{1,32}',role) and
                               type(pin)is dict and set(pin)=={'base','path','bytes','sha256'} and
                               pin['base'] in ('steamvr','home','system'), 'Unknown runtime file root.')
                engine.relative(pin['path'])
                engine.records({pin['path']:{k:pin[k] for k in ('bytes','sha256')}})
        engine.require(set(self.pc['runtime_files'])=={'vrlink','vrpathreg'} and
                       self.pc['runtime_files']['vrlink']['base']=='steamvr' and
                       self.pc['runtime_files']['vrlink']['path']=='drivers/vrlink/bin/win64/driver_vrlink.dll' and
                       self.pc['runtime_files']['vrpathreg']['base']=='steamvr' and
                       self.pc['runtime_files']['vrpathreg']['path']=='bin/win64/vrpathreg.exe' and
                       {'vrlink','compositor','fragment','openvr'}<=set(self.frame['runtime_files']) and
                       self.frame['runtime_files']['vrlink']['base']=='steamvr' and
                       self.frame['runtime_files']['vrlink']['path']=='tools/vrlink/bin/linuxarm64/vrlink',
                       'Runtime paths differ from the reviewed connection.')

    def admit(self, system):
        self.engine.require(self.value['readiness']=='release' and self.pc['reviewed'] and self.frame['reviewed'],
                            'This package is a preview. Checking is available; installation is not enabled yet.')
        self.engine.require(system=='Windows','This package does not contain a reviewed Linux PC connection.')


def private_ipv4(value):
    try:
        address=ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        raise ValueError('A private local IPv4 address is required.') from None
    if not any(address in ipaddress.ip_network(n) for n in ('10.0.0.0/8','172.16.0.0/12','192.168.0.0/16')):
        raise ValueError('A private local IPv4 address is required.')
    return str(address)


def local_address(frame_host, *, resolver=socket.gethostbyname, socket_factory=socket.socket):
    """Ask the OS route table; connect() on a UDP socket sends no packet."""
    peer=private_ipv4(resolver(frame_host))
    with socket_factory(socket.AF_INET,socket.SOCK_DGRAM) as connection:
        connection.connect((peer,9))
        return private_ipv4(connection.getsockname()[0])



def validate_settings_receipt(engine,value,expected_path,section,keys):
    e=engine
    e.require(type(value)is dict and set(value)=={'path','baseline'} and value['path']==str(expected_path) and
              type(value['baseline'])is dict and set(value['baseline'])=={section},'Unknown settings restoration receipt.')
    spec=value['baseline'][section]
    e.require(type(spec)is dict and set(spec)=={'present','keys'} and type(spec['present'])is bool and
              type(spec['keys'])is dict and set(spec['keys'])==set(keys),'Unknown restored setting scope.')
    for key,entry in spec['keys'].items():
        e.require(type(entry)is dict and set(entry)=={'present','value','installed'} and type(entry['present'])is bool,
                  'Unknown restored setting predecessor.')
        if key in ('enableStreamBridge','forceSceneAlphaBlending'):
            e.require(entry['installed'] is True,'Unknown installed switch value.')
        elif key=='controlListenAddress':private_ipv4(entry['installed'])
        else:e.require(type(entry['installed'])is int and 1024<=entry['installed']<=65535,'Unknown installed port.')

class SettingsChange:
    """Restore only our keys, retaining unrelated later SteamVR preferences."""
    def __init__(self, engine, path, edits):
        self.engine,self.path,self.edits=engine,Path(path),edits

    def planned(self):
        e=self.engine
        before=e.read(self.path,2*1024*1024) if self.path.exists() else None
        value=e.parse(before) if before else {}
        baseline={}
        for section,updates in self.edits.items():
            e.require(section not in value or type(value[section])is dict,'Existing SteamVR settings section is invalid.')
            target=value.setdefault(section,{})
            baseline[section]={'present':section in (e.parse(before) if before else {}),'keys':{}}
            for key,next_value in updates.items():
                baseline[section]['keys'][key]={'present':key in target,'value':target.get(key),'installed':next_value}
                target[key]=next_value
        return before,e.encoded(value),baseline

    def apply(self):
        before,after,baseline=self.planned()
        mode=stat.S_IMODE(self.path.stat().st_mode) if self.path.exists() else 0o600
        self.engine.put(self.path,after,before)
        if os.name!='nt':os.chmod(self.path,mode)
        return {'path':str(self.path),'baseline':baseline}

    @staticmethod
    def restore(engine, receipt):
        path=Path(receipt['path']);before=engine.read(path,2*1024*1024);value=engine.parse(before)
        for section,spec in receipt['baseline'].items():
            engine.require(type(value.get(section))is dict,'Changed SteamVR settings preserved.')
            target=value[section]
            for key,entry in spec['keys'].items():
                engine.require(key in target and target[key]==entry['installed'] and
                               type(target[key])is type(entry['installed']), 'Changed SteamVR setting preserved.')
            for key,entry in spec['keys'].items():
                if entry['present']:target[key]=entry['value']
                else:target.pop(key)
            if not target and not spec['present']:value.pop(section)
        mode=stat.S_IMODE(path.stat().st_mode)
        engine.put(path,engine.encoded(value),before)
        if os.name!='nt':os.chmod(path,mode)


class LocalInstall:
    def __init__(self, engine, local):
        self.e,self.local=engine,local
        self.receipt=local.root/'bridge-install.json'

    def state(self):
        if not self.receipt.exists():return None
        e=self.e;v=e.parse(e.read(self.receipt,2*1024*1024,private=True))
        e.require(set(v)=={'schema','bundle','files','settings','registered_before','pair','vrpathreg'} and
                  v['schema']==RECEIPT and re.fullmatch('[0-9a-f]{64}',v['bundle']) and
                  set(v['files'])==PC_FILES,'Unknown PC connection receipt preserved.')
        e.records(v['files'])
        e.require(type(v['registered_before'])is bool and re.fullmatch('[0-9a-f]{32}',v['pair']),'Unknown PC ownership receipt.')
        e.require(type(v['vrpathreg'])is dict and set(v['vrpathreg'])=={'base','path','bytes','sha256'} and v['vrpathreg']['base']=='steamvr' and v['vrpathreg']['path']=='bin/win64/vrpathreg.exe','Unknown registration predecessor.')
        validate_settings_receipt(e,v['settings'],self.settings_path(),SETTINGS_SECTION,('enableStreamBridge','controlListenAddress','controlPort'))
        return v

    def settings_path(self):
        e=self.e;paths=e.parse(e.read(self.local.registry,128*1024));configs=paths.get('config',[])
        e.require(type(configs)is list and 1<=len(configs)<=8 and type(configs[0])is str,
                  'SteamVR settings location could not be confirmed.')
        path=e.no_links(Path(configs[0]))
        e.require(path.is_absolute(),'SteamVR settings location must be absolute.')
        return path/'steamvr.vrsettings'

    def check(self,bundle,pc):
        matches={};e=self.e
        for role,pin in bundle.pc['runtime_files'].items():
            try:
                raw=e.read(Path(pc['steamvr'])/e.relative(pin['path']),pin['bytes'])
                matches[role]=len(raw)==pin['bytes'] and e.sha(raw)==pin['sha256']
            except (OSError,ValueError,TypeError):matches[role]=False
        return bool(matches) and all(matches.values())

    def install(self,bundle,pc,pair,address,port):
        e=self.e;bundle.admit(self.local.system)
        e.require(self.check(bundle,pc),'This SteamVR version is not supported by this package.')
        e.require(not self.local.runner.runtime_running(),'Close SteamVR on this PC before installing or updating the connection.')
        e.require(type(port)is int and 1024<=port<=65535,'Invalid connection port.')
        old=self.state();planned={name:bundle.files[name] for name in PC_FILES}
        tool_pin=bundle.pc['runtime_files']['vrpathreg'];tool=self.local.verify_tool(pc,tool_pin)
        driver=self.local.root/'pc-provider'
        if driver.exists():
            present={str(p.relative_to(self.local.root)).replace('\\','/') for p in driver.rglob('*') if p.is_file() or p.is_symlink()}
            e.require(present<=set(old['files'] if old else {}),'An unknown PC provider is already present; it was preserved.')
        if old:
            e.require(old['bundle']==bundle.pin,'Remove the previous checked connection before installing a different package.')
            missing=[]
            for name,pin in old['files'].items():
                path=self.local.root/e.relative(name)
                if not path.exists():missing.append((path,planned[name]))
                else:e.require(e.sha(e.read(path))==pin['sha256'],'Changed provider files preserved.')
            for path,raw in missing:e.put(path,raw)
            if not pc.get('registered'):
                self.local.register(tool,'adddriver')
                e.require(self.local.inventory()['registered'],'The repaired provider registration could not be confirmed.')
            return {'reason':'repaired' if missing or not pc.get('registered') else 'current',
                    'changed':bool(missing) or not pc.get('registered'),'automatic_restart':False}
        edits=SettingsChange(e,self.settings_path(),{SETTINGS_SECTION:{'enableStreamBridge':True,
              'controlListenAddress':private_ipv4(address),'controlPort':port}})
        edits.planned() # All settings validation before first payload write.
        undo=[];settings=None;registered=False
        pairdir=self.local.root/'pairings';key=pairdir/(pair['pair_id']+'.json');active=pairdir/'active.json'
        pair_raw=e.encoded(pair);active_raw=e.encoded({'schema':e.ACTIVE,'pair_id':pair['pair_id']})
        e.require(not key.exists() and not active.exists(),'Existing pairing credentials require explicit checked repair.')
        try:
            for name,raw in planned.items():
                path=self.local.root/e.relative(name);e.put(path,raw);undo.append((path,raw))
            settings=edits.apply()
            e.put(key,pair_raw);undo.append((key,pair_raw));e.put(active,active_raw);undo.append((active,active_raw))
            if not pc.get('registered'):
                self.local.register(tool,'adddriver');registered=True
                e.require(self.local.inventory()['registered'],'The PC provider registration could not be confirmed.')
            value={'schema':RECEIPT,'bundle':bundle.pin,'files':{n:bundle.value['files'][n] for n in PC_FILES},
                   'settings':settings,'registered_before':bool(pc.get('registered')),'pair':pair['pair_id'],'vrpathreg':tool_pin}
            e.put(self.receipt,e.encoded(value))
        except BaseException:
            if not pc.get('registered'):
                self.local.register(tool,'removedriver')
            if settings:SettingsChange.restore(e,settings)
            for path,raw in reversed(undo):e.remove_owned(path,raw)
            raise
        return {'reason':'installed','changed':True,'automatic_restart':False}

    def uninstall(self,pc):
        e=self.e;state=self.state()
        if state is None:return {'reason':'not_installed','automatic_restart':False}
        e.require(not self.local.runner.runtime_running(),'Close SteamVR on this PC before removing the connection.')
        for name,pin in state['files'].items():
            e.require(e.sha(e.read(self.local.root/e.relative(name)))==pin['sha256'],'Changed provider files preserved.')
        tool=self.local.verify_tool(pc,state['vrpathreg'])
        # Validate conditional settings restoration before unregistering anything.
        raw=e.read(Path(state['settings']['path']),2*1024*1024);current=e.parse(raw)
        for section,spec in state['settings']['baseline'].items():
            e.require(type(current.get(section))is dict and all(k in current[section] and
                      current[section][k]==v['installed'] and type(current[section][k])is type(v['installed'])
                      for k,v in spec['keys'].items()),'Changed SteamVR settings preserved; removal stopped.')
        if not state['registered_before']:self.local.register(tool,'removedriver')
        SettingsChange.restore(e,state['settings'])
        for name in state['files']:e.remove_owned(self.local.root/e.relative(name),e.read(self.local.root/e.relative(name)))
        # Only this session's exact active pointer is removed; keep old user keys.
        active=self.local.root/'pairings/active.json'
        if active.exists():e.remove_owned(active,e.encoded({'schema':e.ACTIVE,'pair_id':state['pair']}))
        e.remove_owned(self.receipt,e.read(self.receipt,2*1024*1024,private=True))
        return {'reason':'uninstalled','automatic_restart':False}


class FrameTransaction:
    """Durable exact-predecessor journal; no service start/stop/restart command."""
    def __init__(self, engine, home, root):
        self.e,self.home,self.root=engine,Path(home),Path(root)
        self.path=self.root/'install.json'

    def owned(self,path):
        e=self.e;path=Path(path).absolute();e.no_links(path.parent)
        e.require(path.is_relative_to(self.home),'Frame setup writes only inside the user home.')
        for parent in reversed((path.parent,*path.parent.parents)):
            if parent==self.home or self.home in parent.parents:
                if parent.exists():
                    info=parent.stat()
                    e.require(info.st_uid==os.getuid() and not info.st_mode&0o022,
                              'A writable or foreign Frame setup directory was preserved.')
        return path

    def state(self):
        if not self.path.exists():return None
        e=self.e;v=e.parse(e.read(self.path,64*1024*1024,private=True))
        e.require(set(v)=={'schema','home','root','bundle','phase','operations','settings','pair'} and
                  v['schema']==RECEIPT and v['home']==str(self.home) and v['root']==str(self.root) and
                  v['phase'] in ('pending','installed','removing') and re.fullmatch('[0-9a-f]{64}',v['bundle']) and
                  re.fullmatch('[0-9a-f]{32}',v['pair']) and type(v['operations'])is list and
                  len(v['operations'])<=96,'Unknown Frame installation journal preserved.')
        allowed={self.home/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf',
                 self.home/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json',
                 self.home/'.config/SteamFrameNativePassthrough/control.json',
                 Path(os.environ.get('XDG_CONFIG_HOME') or self.home/'.config')/'vulkan/implicit_layer.d'/MANIFEST,
                 Path(os.environ.get('XDG_CONFIG_HOME') or self.home/'.config')/'systemd/user/steam.service.d/85-steam-link-native-passthrough.conf'}
        validate_settings_receipt(e,v['settings'],self.home/'.local/share/Steam/config/steamvr.vrsettings','camera',('forceSceneAlphaBlending',))
        e.require(len({o['path'] for o in v['operations']})==len(v['operations']),'Duplicate stored operations.')
        for operation in v['operations']:
            target=Path(operation['path']);self.owned(target)
            e.require((target.is_relative_to(self.root) and target!=self.path) or target in allowed, 'Unknown Frame component scope preserved.')
            e.require(set(operation)=={'path','before','after','mode'} and
                      type(operation['mode'])is int and operation['mode'] in (0o600,0o700),
                      'Unknown Frame operation preserved.')
            for key in ('before','after'):
                value=operation[key]
                e.require(value is None or type(value)is dict and set(value) in
                          ({'kind','bytes'},{'kind','target'}),'Unknown stored predecessor.')
                if value and value['kind']=='file':
                    raw=base64.b64decode(value['bytes'],validate=True)
                    e.require(len(raw)<=e.MAX_FILE,'Stored file exceeds setup limit.')
                elif value:e.require(value['kind']=='link' and type(value['target'])is str and
                                     len(value['target'])<=1024,'Unknown stored link.')
        return v

    def current(self,path):
        path=Path(path)
        if not os.path.lexists(path):return None
        info=path.lstat()
        if stat.S_ISLNK(info.st_mode):
            self.e.require(info.st_uid==os.getuid() and info.st_nlink==1,'Foreign Frame link preserved.')
            return {'kind':'link','target':os.readlink(path)}
        raw=self.e.read(path)
        self.e.require(info.st_uid==os.getuid(),'Foreign Frame file preserved.')
        return {'kind':'file','bytes':base64.b64encode(raw).decode('ascii')}

    def operation(self,path,after,*,mode=0o600,expected=None):
        path=self.owned(path)
        before=self.current(path)
        self.e.require(before==expected,'An unknown Frame component is already present; it was preserved.')
        return {'path':str(path),'before':before,'after':None if after is None else
                {'kind':'file','bytes':base64.b64encode(after).decode('ascii')},'mode':mode}

    def replace(self,operation,reverse=False):
        path=Path(operation['path']);e=self.e;self.owned(path)
        expected,value=(operation['after'],operation['before']) if reverse else (operation['before'],operation['after'])
        observed=self.current(path)
        if observed==value:return # A not-yet-applied operation is safe on rollback.
        e.require(observed==expected,'Changed Frame component preserved; recovery remains available.')
        if observed and observed['kind']=='link':path.unlink()
        if value is None:
            if observed and observed['kind']=='file':e.remove_owned(path,base64.b64decode(observed['bytes']))
        elif value['kind']=='file':
            predecessor=base64.b64decode(observed['bytes']) if observed and observed['kind']=='file' else None
            e.put(path,base64.b64decode(value['bytes']),predecessor)
            os.chmod(path,operation['mode'])
        else:
            e.require(value['kind']=='link','Unknown Frame restore operation.')
            if observed and observed['kind']=='file':e.remove_owned(path,base64.b64decode(observed['bytes']))
            path.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
            os.symlink(value['target'],path)

    def begin(self,bundle,operations,settings,pair):
        e=self.e;e.require(self.state() is None,'A previous Frame connection must be repaired or removed first.')
        value={'schema':RECEIPT,'home':str(self.home),'root':str(self.root),'bundle':bundle,
               'phase':'pending','operations':operations,'settings':settings,'pair':pair}
        e.put(self.path,e.encoded(value)) # Durable rollback authority before activation.
        try:
            for operation in operations:self.replace(operation)
        except BaseException:
            self.rollback()
            raise
        return value

    def commit(self):
        state=self.state();self.e.require(state is not None,'Missing Frame transaction.')
        for operation in state['operations']:
            self.e.require(self.current(Path(operation['path']))==operation['after'],'Frame files changed before commit.')
        previous=self.e.read(self.path,64*1024*1024,private=True)
        state['phase']='installed';self.e.put(self.path,self.e.encoded(state),previous)

    def rollback(self):
        state=self.state()
        if state is None:return
        for operation in reversed(state['operations']):self.replace(operation,reverse=True)
        self.e.remove_owned(self.path,self.e.read(self.path,64*1024*1024,private=True))


def layer_manifest(library):
    return {'file_format_version':'1.1.2','layer':{'name':LAYER,'type':'GLOBAL',
            'library_path':str(library),'api_version':'1.1.0','implementation_version':'1',
            'description':'Steam Link Native Passthrough',
            'functions':{'vkGetInstanceProcAddr':'framebridge_vkGetInstanceProcAddr',
                         'vkGetDeviceProcAddr':'framebridge_vkGetDeviceProcAddr',
                         'vkNegotiateLoaderLayerInterfaceVersion':'framebridge_vkNegotiateLoaderLayerInterfaceVersion'},
            'disable_environment':{'SFNP_STREAM_BRIDGE_DISABLE':'1'}}}


def generate_shaders(engine,bundle,original,stage):
    """Only owned pinned Python is shipped; native-derived output stays local."""
    e=engine;stage=Path(stage)
    for name in bundle.frame['files']:
        if name in TOOLS|PROFILES:
            path=stage/Path(name).relative_to('frame')
            e.put(path,bundle.files[name])
    previous=list(sys.path)
    names={Path(name).stem for name in TOOLS}
    loaded={name:sys.modules.pop(name) for name in names if name in sys.modules}
    try:
        sys.path.insert(0,str(stage/'tools'))
        modules=[]
        for filename in ('native_fragment_stream_coverage.py','native_fragment_scene_presenter.py'):
            name='sfnp_install_'+filename[:-3]
            spec=importlib.util.spec_from_file_location(name,stage/'tools'/filename)
            module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);modules.append(module)
        coverage,_=modules[0].transform(original)
        legacy,_=modules[1].transform_robust(original,True,1)
        outputs={'coverage.spv':coverage,'legacy.spv':legacy}
        for name,raw in outputs.items():
            e.require(len(raw)==GENERATED[name]['bytes'] and e.sha(raw)==GENERATED[name]['sha256'],
                      'The locally generated shader differs from the reviewed output.')
        return outputs
    finally:
        sys.path[:]=previous
        for name,module in list(sys.modules.items()):
            filename=getattr(module,'__file__',None)
            if filename and Path(filename).is_relative_to(stage):sys.modules.pop(name,None)
        sys.modules.update(loaded)


class StartupHealth:
    """Existing read-only process/UI health, plus a separate user picture verdict."""
    def __init__(self, module, home):self.module,self.home=module,Path(home)

    def baseline(self):
        source=self.module.NativeEvidenceSource(self.home)
        return {'baseline':source.baseline(),'checkpoint':source.checkpoint()}

    def check(self,receipt):
        source=self.module.NativeEvidenceSource(self.home);source.resume(receipt['checkpoint'])
        result=self.module.wait_ready(source,receipt['baseline'],timeout=20,poll_seconds=.25,
                                      stable_seconds=1,no_progress_seconds=10)
        # A green service, process tree or even fresh UI logs are not a pixel verdict.
        return {'schema':'sfnp-install-startup-health-v1','software_ready':result.get('status')=='ready',
                'headset_picture_verified':False,'status':result.get('status'),
                'reason':str(result.get('reason') or '')[:512],
                'process_identities':result.get('process_identities',{}),
                'automatic_restart':False,'automatic_rollback':False}


def offer_undo(health,picture_ok):
    """Pure choice policy; callers present a choice, never undo automatically."""
    return not (health.get('software_ready') is True and picture_ok is True)


class FrameInstall:
    def __init__(self, engine, *, home=None, steamvr=Path('/opt/steamvr'), root=None,
                 config=None, run=None, machine=platform.machine):
        self.e=engine;self.home=Path.home() if home is None else Path(home)
        self.steamvr=Path(steamvr)
        base=Path(os.environ.get('XDG_DATA_HOME') or self.home/'.local/share')
        self.root=Path(root) if root else base/'SteamLinkNativePassthrough/stream-bridge'
        self.config=Path(config) if config else Path(os.environ.get('XDG_CONFIG_HOME') or self.home/'.config')
        self.tx=FrameTransaction(engine,self.home,self.root)
        self.run=run or engine.Runner().run;self.machine=machine

    def system(self,*args):
        code,raw=self.run(['/usr/bin/systemctl','--user',*args],timeout=10)
        self.e.require(code==0,'The Frame user service could not be checked. Nothing was restarted.')
        return raw.decode('utf-8')

    def cold(self,*,steam=False):
        raw=self.system('show','steamvr.service','--property=ActiveState','--value').strip()
        self.e.require(raw in ('inactive','failed'),'Stop SteamVR on the Frame before changing its loaded connection files.')
        if steam:
            state=self.system('show','steam.service','--property=ActiveState','--value').strip()
            self.e.require(state in ('inactive','failed'),'Stop Steam on the Frame before removing its loaded launch selector. Recovery files were retained.')

    def runtime(self,bundle):
        good=True;original=None
        for role,pin in bundle.frame['runtime_files'].items():
            base={'steamvr':self.steamvr,'home':self.home,'system':Path('/')}[pin['base']]
            try:
                raw,_=self.e.read_public_reference(base/self.e.relative(pin['path']),pin['bytes'])
                match=len(raw)==pin['bytes'] and self.e.sha(raw)==pin['sha256']
                if role=='fragment' and match:original=raw
                good=good and match
            except (OSError,ValueError):good=False
        return good,original

    def inventory(self,bundle):
        state=self.tx.state();compatible,_=self.runtime(bundle)
        current=state is not None and state['bundle']==bundle.pin and state['phase']=='installed'
        if current:
            try:current=all(self.tx.current(Path(o['path']))==o['after'] for o in state['operations'])
            except (OSError,ValueError):current=False
        return {'architecture':self.machine(),'runtime_compatible':compatible,
                'installed':state is not None,'files_current':current,
                'legacy_installed':(self.home/'.config/SteamFrameNativePassthrough/install.json').exists(),
                'active_pair':state['pair'] if state else None,'automatic_restart':False}

    def legacy_operations(self):
        """Suspend exact old activation; preserve its payload, receipt and choice."""
        e=self.e;receipt=self.home/'.config/SteamFrameNativePassthrough/install.json'
        dropin=self.home/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf'
        link=self.home/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json'
        control=self.home/'.config/SteamFrameNativePassthrough/control.json'
        if not receipt.exists():
            e.require(not os.path.lexists(dropin) and not os.path.lexists(link),'An unknown old activation was preserved.')
            return []
        value=e.parse(e.read(receipt,128*1024,private=True))
        data=self.home/'.local/share/SteamFrameNativePassthrough'
        e.require(set(value)=={'schema','home','data','manifest_link','package_sha256','files'} and
                  value['schema']=='sfnp-user-install-v1' and value['home']==str(self.home) and
                  value['data']==str(data) and type(value['files'])is dict and 1<=len(value['files'])<=32,
                  'The old Frame installation could not be verified; it was preserved.')
        for name,pin in value['files'].items():
            path=Path(name)
            e.require(path.is_relative_to(data) or path==dropin,'Unknown old Frame file ownership.')
            e.require(e.sha(e.read(path))==pin,'An old Frame component changed; it was preserved.')
        operations=[]
        if dropin.exists():
            e.require(str(dropin) in value['files'],'Unowned old service settings preserved.')
            operations.append(self.tx.operation(dropin,None,expected=self.tx.current(dropin)))
        if os.path.lexists(link):
            expected={'kind':'link','target':str(Path('/run/user')/str(os.getuid())/'steam-frame-native-passthrough/receiver.json')}
            e.require(value['manifest_link']==expected['target'],'Unknown old manifest target preserved.')
            operations.append(self.tx.operation(link,None,expected=expected))
        if control.exists():
            old=e.read(control,4096,private=True);v=e.parse(old)
            e.require(v.get('schema')=='sfnp-user-control-v1' and type(v.get('enabled'))is bool,'Unknown old preference preserved.')
            v['enabled']=False
            operations.append(self.tx.operation(control,e.encoded(v),expected=self.tx.current(control)))
        return operations

    @staticmethod
    def systemd(value):
        if not isinstance(value,str) or not value or len(value)>4096 or any(ord(c)<32 for c in value):
            raise ValueError('Unsafe service value.')
        return '"'+value.replace('\\','\\\\').replace('"','\\"').replace('%','%%')+'"'

    def dropin(self):
        # Keep all pre-existing preload tokens, including their delimiters. Do
        # not change LD_AUDIT or any other service environment variable.
        raw=self.system('show','steam.service','--property=Environment','--value')
        self.e.require(len(raw)<=65536,'Service environment exceeded setup limit.')
        tokens=shlex.split(raw,posix=True);values={}
        for token in tokens:
            self.e.require('=' in token,'Unsupported service environment quoting.')
            name,value=token.split('=',1)
            self.e.require(name not in values,'Duplicate service environment preserved.')
            values[name]=value
        selector=str(self.root/'libsfnp_stream_launch.so')
        old=values.get('LD_PRELOAD','')
        self.e.require(selector not in re.split('[ :\t]+',old),'A selector is already configured; repair its owned installation.')
        preload=old+(':' if old else '')+selector
        config=str(self.root/'launch.conf')
        return ('[Service]\nEnvironment='+self.systemd('LD_PRELOAD='+preload)+'\nEnvironment='+
                self.systemd('SFNP_STREAM_BRIDGE_CONFIG='+config)+'\n').encode()

    def plan(self,bundle,pair,address,port):
        e=self.e;bundle.admit('Windows')
        e.require(self.machine().lower() in ('aarch64','arm64'),'The selected device is not a supported Steam Frame.')
        good,original=self.runtime(bundle);e.require(good and original is not None,'The Frame runtime does not match this package.')
        self.cold()
        e.require(type(port)is int and 1024<=port<=65535,'Invalid connection port.')
        address=private_ipv4(address)
        e.require(type(pair)is dict and set(pair)=={'schema','pair_id','key_hex','role','protocol_major'} and
                  pair['schema']==e.PAIR and pair['role']=='receiver' and pair['protocol_major']==1 and
                  re.fullmatch('[0-9a-f]{32}',pair['pair_id']) and re.fullmatch('[0-9a-f]{64}',pair['key_hex']),
                  'Invalid private pairing data.')
        for path in (self.root,self.root/'libsfnp_stream_launch.so',self.root/LIBRARY):
            # The tiny native launcher consumes four bounded printable paths.
            e.require(path.is_absolute() and not any(ord(c)<=32 or ord(c)>126 or c in ':=' for c in str(path)),
                      'The Frame installation directory is not supported by the launch selector.')
        e.require(not self.root.exists() or stat.S_IMODE(self.root.stat().st_mode)==0o700,
                  'The Frame connection directory must be private.')
        outputs={}
        self.tx.owned(self.root/'prepared-file')
        self.root.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
        with tempfile.TemporaryDirectory(prefix='prepare-',dir=self.root.parent) as stage:
            outputs=generate_shaders(e,bundle,original,Path(stage))
        library=bundle.files['frame/'+LIBRARY];selector=bundle.files['frame/libsfnp_stream_launch.so']
        for raw in (library,selector):
            e.require(len(raw)>=64 and raw[:7]==b'\x7fELF\x02\x01\x01' and raw[18:20]==b'\xb7\x00',
                      'The Frame package contains an unsupported native library.')
        files={LIBRARY:library,'libsfnp_stream_launch.so':selector,**outputs,
               'receiver.conf':MARKER+address.encode()+b'\n'+str(port).encode()+b'\n',
               'launch.conf':b'SFNP_STREAM_LAUNCH/1\n'+str(self.steamvr/bundle.frame['runtime_files']['vrlink']['path']).encode()+b'\n'+
                    str(self.root/'libsfnp_stream_launch.so').encode()+b'\n'+str(self.root/LIBRARY).encode()+b'\n',
               'pairings/'+pair['pair_id']+'.json':e.encoded(pair),
               'pairings/active.json':e.encoded({'schema':e.ACTIVE,'pair_id':pair['pair_id']}),
               # Publish opt-in after all code/configuration/pairing writes.
               'receiver.enable':MARKER}
        for name in bundle.frame['files']:
            if name in TOOLS|PROFILES:files[str(Path(name).relative_to('frame'))]=bundle.files[name]
        operations=self.legacy_operations()
        for name,raw in files.items():operations.append(self.tx.operation(self.root/name,raw))
        manifest=self.config/'vulkan/implicit_layer.d'/MANIFEST
        operations.append(self.tx.operation(manifest,e.encoded(layer_manifest(self.root/LIBRARY))))
        dropin=self.config/'systemd/user/steam.service.d/85-steam-link-native-passthrough.conf'
        operations.append(self.tx.operation(dropin,self.dropin()))
        settings=self.home/'.local/share/Steam/config/steamvr.vrsettings'
        change=SettingsChange(e,settings,{'camera':{'forceSceneAlphaBlending':True}})
        before,after,baseline=change.planned()
        # Settings get a separate key-level receipt; do not restore the whole
        # SteamVR file after the player changes unrelated preferences later.
        return operations,{'path':str(settings),'baseline':baseline},(before,after)

    def install(self,bundle,pair,address,port):
        e=self.e;current=self.tx.state()
        if current:
            e.require(current['phase']=='installed' and current['bundle']==bundle.pin and current['pair']==pair['pair_id'],
                      'Repair or remove the previous Frame transaction before replacing it.')
            if self.inventory(bundle)['files_current']:return {'reason':'current','automatic_restart':False}
            self.cold(steam=True)
            missing=[]
            for operation in current['operations']:
                observed=self.tx.current(Path(operation['path']))
                if observed==operation['after']:continue
                e.require(observed is None and operation['before'] is None and operation['after'] is not None,
                          'Changed Frame files were preserved; only missing owned components can be repaired.')
                missing.append(operation)
            for operation in missing:self.tx.replace(operation)
            self.system('daemon-reload')
            return {'reason':'repaired','automatic_restart':False,'manual_restart_required':True}
        operations,settings,(before,after)=self.plan(bundle,pair,address,port)
        self.tx.begin(bundle.pin,operations,settings,pair['pair_id'])
        try:
            settings_path=Path(settings['path']);mode=stat.S_IMODE(settings_path.stat().st_mode) if settings_path.exists() else 0o600
            e.put(settings_path,after,before)
            os.chmod(settings_path,mode)
            self.system('daemon-reload')
        except BaseException:
            self.undo()
            raise
        return {'reason':'prepared','pair_id':pair['pair_id'],'automatic_restart':False,
                'manual_restart_required':True,'coverage_operational':False}

    def undo(self):
        state=self.tx.state()
        if state is None:return {'reason':'not_installed','automatic_restart':False}
        self.cold(steam=True)
        # Revoke new activation first; never restore the old layer while the new
        # compositor is still using the same native descriptor path.
        marker=self.root/'receiver.enable'
        if marker.exists():
            expected=next(o['after'] for o in state['operations'] if o['path']==str(marker))
            self.e.require(self.tx.current(marker)==expected,'Changed enable marker preserved.')
            self.e.remove_owned(marker,MARKER)
        settings=state['settings']
        try:SettingsChange.restore(self.e,settings)
        except self.e.SetupError:
            # Pending transaction may have failed before the settings write.
            current=self.e.parse(self.e.read(Path(settings['path']),2*1024*1024))
            for section,spec in settings['baseline'].items():
                values=current.get(section,{})
                self.e.require(all((key in values)==entry['present'] and
                    (not entry['present'] or values[key]==entry['value']) for key,entry in spec['keys'].items()),
                    'The camera preference changed; it was preserved and recovery remains available.')
        self.tx.rollback()
        self.system('daemon-reload')
        return {'reason':'uninstalled','automatic_restart':False,'manual_restart_required':True}


REMOTE_BOOTSTRAP = r'''
import base64,json,sys,types
r=json.loads(sys.stdin.buffer.read(94371841))
if not isinstance(r,dict):raise ValueError('request')
e=types.ModuleType('sfnp_remote_engine');e.__file__='<verified-setup-engine>'
exec(compile(base64.b64decode(r.pop('_engine'),validate=True),e.__file__,'exec'),e.__dict__)
b=types.ModuleType('sfnp_remote_bridge');b.__file__='<verified-bridge-engine>';sys.modules[b.__name__]=b
exec(compile(base64.b64decode(r.pop('_bridge'),validate=True),b.__file__,'exec'),b.__dict__)
try:
 result=b.remote_request(e,r)
 print(json.dumps(result,sort_keys=True))
except Exception:
 print(json.dumps({'error':'Frame setup refused; recovery records and existing unknown files are preserved.'}))
 sys.exit(2)
'''


def load_module(raw,name):
    module=type(sys)(name);module.__file__='<verified-'+name+'>';sys.modules[name]=module
    exec(compile(raw,module.__file__,'exec'),module.__dict__)
    return module



class ReadOnlyView:
    def __init__(self,engine,value):
        e=engine;e.require(type(value)is dict and set(value)=={'schema','readiness','bridge_protocol','pc','frame','files'} and value['schema']==SCHEMA,'Unknown inventory manifest.')
        e.protocol(value['bridge_protocol']);e.records(value['files'])
        self.pin=e.sha(e.encoded(value));self.frame=value['frame'];self.value=value
        e.require(type(self.frame)is dict and {'runtime_files','files','reviewed','generated'}==set(self.frame),'Unknown Frame inventory shape.')
        pins=self.frame['runtime_files'];e.require(type(pins)is dict and 1<=len(pins)<=32,'Bounded runtime inventory required.')
        for pin in pins.values():
            e.require(type(pin)is dict and set(pin)=={'base','path','bytes','sha256'} and pin['base'] in ('steamvr','home','system'),'Unknown inventory root.')
            e.relative(pin['path']);e.records({pin['path']:{k:pin[k] for k in ('bytes','sha256')}})

def remote_request(engine,request,*,unit=None):
    e=engine;unit=unit or FrameInstall(e)
    command=request['command']
    if command in ('bridge-inventory','bridge-health-baseline','bridge-health'):
        view=ReadOnlyView(e,request['manifest'])
        if command=='bridge-inventory':return unit.inventory(view)
        tool=request.get('health_source');name='frame/tools/compositor_startup_health.py';pin=view.value['files'][name]
        e.require(type(tool)is str,'The read-only startup checker is missing.')
        source=base64.b64decode(tool,validate=True)
        e.require(len(source)==pin['bytes'] and e.sha(source)==pin['sha256'],'The startup checker changed.')
        observer=StartupHealth(load_module(source,'sfnp_install_health'),unit.home)
        return observer.baseline() if command=='bridge-health-baseline' else observer.check(request['baseline'])
    if command=='bridge-undo':return unit.undo()
    if command=='bridge-commit':unit.tx.commit();return {'reason':'committed','automatic_restart':False}
    files=request.get('files',{});e.require(type(files)is dict and 1<=len(files)<=64,'Bounded package inventory required.')
    # RPC payload staging has no service or library side effects. Its complete
    # exact manifest is checked again on the receiving machine before use.
    with tempfile.TemporaryDirectory(prefix='sfnp-received-') as temporary:
        root=Path(temporary);manifest=request['manifest'];total=0
        for name,item in files.items():
            path=root/e.relative(name);raw=base64.b64decode(item['data'],validate=True);total+=len(raw)
            e.require(total<=e.MAX_TOTAL and len(raw)==item['bytes'] and e.sha(raw)==item['sha256'],
                      'A transferred package file changed.')
            e.put(path,raw)
        e.put(root/'stream-bridge-bundle.json',e.encoded(manifest))
        bundle=Bundle(root,e)
        if command=='bridge-prepare':return unit.install(bundle,request['pairing'],request['address'],request['port'])
        raise e.SetupError('Unknown fixed Frame operation.')


class Remote:
    def __init__(self,engine,address,login='steamos',*,runner=None,interactive=False):
        self.e=engine;self.address=engine.host(address);self.login=engine.user(login)
        self.runner=runner or engine.Runner();self.interactive=interactive

    def call(self,request):
        e=self.e
        request={**request,'_engine':base64.b64encode(Path(e.__file__).read_bytes()).decode(),
                 '_bridge':base64.b64encode(Path(__file__).read_bytes()).decode()}
        args=e.ssh_args(self.address,self.login,interactive=self.interactive)
        args+=['python3 -B -c '+shlex.quote(REMOTE_BOOTSTRAP)]
        code,raw=self.runner.run(args,e.encoded(request),interactive=self.interactive,timeout=90)
        e.require(code==0,'The Frame operation could not be confirmed. Run Check; retained recovery records can undo this installation.')
        result=e.parse(raw);e.require(not result.get('error'),'The Frame refused this operation. Existing files and recovery records were preserved.')
        return result


def request(bundle,command,**values):
    result={'command':command,'manifest':bundle.value,**values}
    if command=='bridge-prepare':
        result['files']={name:{**bundle.value['files'][name],
            'data':base64.b64encode(raw).decode()} for name,raw in bundle.files.items()}
    if command in ('bridge-health-baseline','bridge-health'):
        result['health_source']=base64.b64encode(bundle.files['frame/tools/compositor_startup_health.py']).decode()
    return result


def run(args,engine,local,*,input_fn=input,discover_frame=None,remote_factory=None):
    """Called only when this exact package contains the new bundle manifest."""
    e=engine;bundle=Bundle(args.bundle,e);pc=local.inventory();installed=LocalInstall(e,local)
    interactive=not args.check and sys.stdin.isatty()
    ui=e.load_presentation().Presenter(input_fn=input_fn,lang=args.lang,input_enabled=True) if interactive else None
    if ui:ui.banner(inspection=False);ui.status('pc_ready' if installed.check(bundle,pc) else 'pc_partial','info')
    remote=frame=None;address=args.frame
    factory=remote_factory or (lambda value,login,**kw:Remote(e,value,login,**kw))
    if interactive and not address:
        def validate(candidate,*,interactive=False):
            try:
                peer=factory(candidate,args.user,interactive=interactive)
                observed=peer.call(request(bundle,'bridge-inventory'))
                if observed.get('architecture') in ('aarch64','arm64') and (observed.get('runtime_compatible') or observed.get('installed') or observed.get('legacy_installed')):
                    return peer,observed
            except (OSError,ValueError):pass
            return None
        presentation=e.load_presentation();finder=discover_frame or presentation.find_frame
        found=finder(validate,cached_hosts=e.remembered_frame_hosts(local),presenter=ui)
        if found:address=found.host;remote,frame=found.value
    if address and frame is None:
        remote=factory(address,args.user,interactive=interactive)
        with ui.suspend() if ui else e.nullcontext():frame=remote.call(request(bundle,'bridge-inventory'))
    available=bundle.value['readiness']=='release' and bundle.pc['reviewed'] and bundle.frame['reviewed'] and local.system=='Windows'
    state=installed.state()
    report={'schema':SCHEMA,'bridge_available':available,'automatic_restart':False,
            'pc':{'compatible':installed.check(bundle,pc),'installed':state is not None},'frame':frame}
    if args.check or args.dry_run:
        print(json.dumps(report,sort_keys=True));return 0
    if ui:
        if frame is not None:ui.status('receiver_ready' if frame.get('runtime_compatible') else 'receiver_changed','info')
        ui.status('bridge_available' if available else 'bridge_unavailable','info')
    actions=['check','exit']
    if frame is not None and (state or frame.get('installed')):actions.insert(0,'uninstall')
    if available and frame is not None and report['pc']['compatible'] and frame.get('runtime_compatible'):
        if state is None:actions.insert(0,'install')
        elif state['bundle']==bundle.pin:actions.insert(0,'repair')
    action=args.action or (ui.choose_action(actions,recommended='check') if ui else None)
    if action in (None,'exit','check'):return 0
    e.require(interactive and action in actions,'That action is not available for this checked package.')
    if action=='uninstall':
        ui.say('bridge_remove_plan')
        if not ui.confirm_uninstall():return 0
        # The native processes must be stopped by the user first. Both sides
        # retain ownership receipts if an operation cannot finish safely.
        remote.call({'command':'bridge-undo'});result=installed.uninstall(pc)
        ui.say('bridge_removed');return 0
    bundle.admit(local.system)
    ui.say('bridge_install_plan')
    if ui.ask('bridge_confirm').strip().lower() not in ('y','yes','j','ja'):return 0
    pair=local.pairing(state['pair']) if state else {'schema':e.PAIR,'pair_id':secrets.token_hex(16),'key_hex':secrets.token_hex(32),
          'role':'producer','protocol_major':1}
    address_to_bind=local_address(address)
    port=47631
    # Baseline belongs to the current service invocation. The later gate must
    # observe a new successful start, rather than reusing these old UI markers.
    with ui.suspend():baseline=remote.call(request(bundle,'bridge-health-baseline'))
    local.apply_started=True
    try:
        with ui.suspend():frame_result=remote.call(request(bundle,'bridge-prepare',pairing={**pair,'role':'receiver'},address=address_to_bind,port=port))
        pc_result=installed.install(bundle,pc,pair,address_to_bind,port)
        with ui.suspend():remote.call({'command':'bridge-commit'})
    except BaseException:
        # A failed post-install start never triggers an automatic rollback.
        # Incomplete atomic file writes retain/restore their own predecessors.
        # Undo is OFFERED after an uncertain cross-device installation.

        # This includes an uncertain SSH response; receipts remain available.
        ui.say('bridge_partial')
        if ui.choose_action(['uninstall','exit'],recommended='uninstall')=='uninstall' and ui.confirm_uninstall():
            with ui.suspend():remote.call({'command':'bridge-undo'})
            installed.uninstall(pc)
            ui.say('bridge_removed')
            return 0
        raise
    if frame_result.get('reason')=='current' and pc_result.get('reason')=='current':
        ui.say('bridge_current');return 0
    e.remember_frame(local,address,args.user)
    log=e.write_report(local,action,report,{'reason':'installed','coverage_operational':False,'automatic_restart':False})
    ui.path(log)
    ui.say('bridge_installed')
    if ui.ask('bridge_started').strip().lower() in ('y','yes','j','ja'):
        with ui.suspend():health=remote.call(request(bundle,'bridge-health',baseline=baseline))
        ui.say('bridge_health_ok' if health['software_ready'] else 'bridge_health_bad')
        picture=ui.ask('bridge_picture').strip().lower() in ('y','yes','j','ja')
        e.write_report(local,action,report,{'startup_health':health,'user_picture_usable':picture,'automatic_restart':False})
        if offer_undo(health,picture):
            choice=ui.choose_action(['uninstall','exit'],recommended='uninstall')
            if choice=='uninstall' and ui.confirm_uninstall():
                ui.say('bridge_stop_first');ui.ask('bridge_continue')
                with ui.suspend():remote.call({'command':'bridge-undo'})
                installed.uninstall(pc);ui.say('bridge_removed')
    return 0
