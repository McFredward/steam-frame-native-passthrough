"""Exact-package full-resolution setup with confirmed lifecycle orchestration.

This module is loaded by the verified setup entry. Prototype manifests provide
inventory only. Source identities and successful software tests do not turn a
prototype into an installable release.
"""
from __future__ import annotations
from contextlib import contextmanager
import base64
import ast
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


def fixed_checks(*sources):
    """Expose only our authored checks, never an arbitrary remote exception."""
    result={}
    for source in sources:
        for node in ast.walk(ast.parse(source)):
            if not isinstance(node,ast.Call):continue
            name=node.func.attr if isinstance(node.func,ast.Attribute) else node.func.id if isinstance(node.func,ast.Name) else None
            index=1 if name=='require' else 0 if name in ('SetupError','error') else None
            if index is not None and len(node.args)>index and isinstance(node.args[index],ast.Constant) and type(node.args[index].value)is str:
                message=node.args[index].value
                result['check_'+hashlib.sha256(message.encode()).hexdigest()[:16]]=message
    return result


def failure_record(engine,error,checks):
    """Only exact local-source check IDs and OS error numbers leave the Frame."""
    code='setup_refused'
    if isinstance(error,engine.SetupError):
        candidate='check_'+hashlib.sha256(str(error).encode()).hexdigest()[:16]
        if candidate in checks:code=candidate
    elif isinstance(error,OSError):code='os_error'
    elif isinstance(error,(ValueError,KeyError,TypeError)):code='invalid_data'
    return {'reason':code,'errno':error.errno if isinstance(error,OSError) and type(error.errno)is int else None}


def load_helper(engine,name):
    cached=sys.modules.get('sfnp_'+name)
    if cached is not None and getattr(cached,'_sfnp_verified_helper',False):return cached
    path=engine.no_links(Path(__file__).with_name(name+'.py'))
    return load_module(path.read_bytes(),'sfnp_'+name)

def local_camera_source():
    root=Path(__file__).resolve().parents[1]
    # Source checkout and extracted public bundle have different tool roots.
    for path in (root/'frame'/CAMERA_TOOL,root/CAMERA_TOOL):
        if path.is_file():return path.read_bytes()
    raise FileNotFoundError('The checked camera helper is missing from this setup package.')


SCHEMA = 'sfnp-stream-bridge-bundle-v1'
RECEIPT = 'sfnp-stream-bridge-install-v1'
MARKER = b'SFNP_STREAM_RECEIVER/1\n'
NAME = 'steam_link_native_passthrough'
LAYER = 'VK_LAYER_SFNP_stream_bridge'
LIBRARY = 'libVkLayer_steam_link_bridge.so'
MANIFEST = 'VkLayer_SFNP_stream_bridge.json'
SETTINGS_SECTION = 'driver_steam_link_native_passthrough'
SESSION_POLICY = 'session-managed-v1'
CAMERA_CONFIG = 'camera-session.json'
CAMERA_HOOK = '85-steam-link-native-passthrough-camera.conf'
CAMERA_TOOL = 'tools/stream_bridge_camera.py'
PC_FILES = {'pc-provider/driver.vrdrivermanifest',
            'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll'}
TOOLS = frozenset('frame/tools/'+name+'.py' for name in (
    'app_payload_probe','compositor_startup_health','native_fragment_count1_interface',
    'native_fragment_ssa_probe','native_fragment_stream_coverage','pixel_payload_recovery','stream_bridge_camera'))
PROFILES = frozenset()
GENERATED = {'coverage.spv': {'bytes':7188,'sha256':'316ceb1cc3c50a6eef9c6e176d1001d1afd61e0747b628d87728439c3d2bfc6e'}}
# Removal/recovery allowlist only. These files are never generated, installed or
# selected by this version. An owned update removes them while retaining their
# exact predecessors in the transaction so explicit Undo remains safe.
RETIRED_FRAME_FILES = frozenset({'legacy.spv',
    'tools/native_fragment_atlas_probe.py','tools/native_fragment_packed_output.py',
    'tools/native_fragment_r1_presenter.py','tools/native_fragment_scene_presenter.py',
    'docs/protocols/scene-contribution-atlas-v1.json',
    'docs/protocols/scene-contribution-atlas-v3.json'})


def patch_version(value):
    """Validate bounded SemVer metadata; file hashes remain install authority."""
    if value is None:return None
    if type(value)is not str or len(value)>80 or not re.fullmatch(
            r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?',value):
        raise ValueError('Invalid semantic patch version.')
    pre=value.split('+',1)[0].partition('-')[2]
    if any(p.isdigit() and len(p)>1 and p[0]=='0' for p in pre.split('.')):
        raise ValueError('Invalid semantic patch version.')
    return value


def older_version(target,current):
    """Compare known SemVer precedence, never guess an old unversioned build."""
    if target is None or current is None:return False
    def parts(value):
        core,_,pre=patch_version(value).split('+',1)[0].partition('-')
        return tuple(map(int,core.split('.'))),pre.split('.') if pre else []
    a,ap=parts(target);b,bp=parts(current)
    if a!=b:return a<b
    if not ap or not bp:return bool(ap) and not bp
    for x,y in zip(ap,bp):
        if x==y:continue
        if x.isdigit() and y.isdigit():return int(x)<int(y)
        if x.isdigit()!=y.isdigit():return x.isdigit()
        return x<y
    return len(ap)<len(bp)


def version_fields(value):
    """Absent historical metadata stays absent in receipts and checkpoints."""
    if 'version' in value and value['version'] is None:raise ValueError('Invalid semantic patch version.')
    return {'version':patch_version(value['version'])} if 'version' in value else {}


def blocked_version_update(target,current):
    return current is not None and (target is None or older_version(target,current))


def version_status(state,target):
    return {'installed':state is not None,'installed_version':patch_version(state.get('version')) if state else None,
            'target_version':patch_version(target.get('version'))}


class Bundle:
    def __init__(self, root, engine):
        self.engine = engine
        self.root = engine.no_links(root)
        raw = engine.read(self.root / 'stream-bridge-bundle.json',128*1024)
        self.value = engine.parse(raw)
        self.pin = engine.sha(engine.encoded(self.value))
        v = self.value
        engine.require(set(v)-{'version'}=={'schema','readiness','bridge_protocol','pc','frame','files'} and
                       v['schema']==SCHEMA and v['readiness'] in ('prototype','release'),
                       'Unknown full-resolution setup package.')
        version_fields(v)
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
        engine.require(type(self.pc)is dict and set(self.pc)-{'version'}=={'reviewed','platform','files','runtime_files'} and
                       self.pc['platform']=='windows-x86_64' and type(self.pc['reviewed'])is bool and
                       type(self.pc['files'])is list and set(self.pc['files'])==PC_FILES,
                       'Unsupported PC connection package.')
        engine.require(type(self.frame)is dict and set(self.frame)-{'version'}=={'reviewed','files','runtime_files','generated'} and
                       type(self.frame['reviewed'])is bool and type(self.frame['files'])is list and
                       len(self.frame['files'])==len(set(self.frame['files'])) and
                       set(self.frame['files'])==TOOLS|PROFILES|{'frame/'+LIBRARY,'frame/libsfnp_stream_launch.so'} and
                       self.frame['generated']==GENERATED and
                       {'frame/'+LIBRARY,'frame/tools/native_fragment_stream_coverage.py',
                        'frame/tools/native_fragment_count1_interface.py','frame/libsfnp_stream_launch.so',
                        'frame/tools/compositor_startup_health.py'}<=set(self.frame['files']) and
                       all(n in PROFILES|{'frame/'+LIBRARY,'frame/libsfnp_stream_launch.so'} or re.fullmatch(r'frame/tools/[a-z0-9_]+[.]py',n)
                           for n in self.frame['files']), 'Unsupported Frame connection package.')
        engine.require(set(self.files)==set(self.pc['files'])|set(self.frame['files']),
                       'Unknown executable or shader payload in package.')
        provider=engine.parse(self.files['pc-provider/driver.vrdrivermanifest'])
        engine.require(provider.get('name')==NAME and provider.get('alwaysActivate') is True,
                       'Unsupported owned OpenVR provider manifest.')
        for side in (self.pc,self.frame):
            version_fields(side)
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
    e.require(type(value)is dict and set(value) in ({'path','baseline'},{'path','baseline','policy'}) and
              ('policy' not in value or section=='camera' and value['policy']==SESSION_POLICY) and value['path']==str(expected_path) and
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
        # Session-owned changes have separate live leases. Installation itself
        # never owns or overwrites the user's persistent camera preference.
        if receipt.get('policy')==SESSION_POLICY:return
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
        return self.validate_state(v)

    def validate_state(self,v):
        e=self.e
        e.require(set(v)-{'version'}=={'schema','bundle','files','settings','registered_before','pair','vrpathreg'} and
                  v['schema']==RECEIPT and re.fullmatch('[0-9a-f]{64}',v['bundle']) and
                  set(v['files'])==PC_FILES,'Unknown PC connection receipt preserved.')
        version_fields(v)
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

    def checkpoint(self,pair_id=None):
        helper=load_helper(self.e,'steam_link_setup_checkpoint')
        paths={self.local.root/name for name in PC_FILES}|{self.receipt,self.settings_path(),self.local.root/'pairings/active.json'}
        def allowed(path):
            return path in paths or path.parent==self.local.root/'pairings' and re.fullmatch('[0-9a-f]{32}[.]json',path.name) is not None
        return helper.Checkpoint(self.e,self.local.root/'setup-restore.json',allowed,
            lambda p:helper.read_value(self.e,p),lambda p,old,new:helper.replace_value(self.e,p,old,new))

    def install(self,bundle,pc,pair,address,port,*,backup=False):
        e=self.e;bundle.admit(self.local.system)
        e.require(self.check(bundle,pc),'This SteamVR version is not supported by this package.')
        e.require(not self.local.runner.runtime_running(),'Close SteamVR on this PC before installing or updating the connection.')
        e.require(type(port)is int and 1024<=port<=65535,'Invalid connection port.')
        old=self.state();planned={name:bundle.files[name] for name in PC_FILES}
        e.require(not blocked_version_update(bundle.pc.get('version'),old.get('version') if old else None),
                  'The installed PC patch is newer or the package is unversioned; downgrade was not applied.')
        tool_pin=bundle.pc['runtime_files']['vrpathreg'];tool=self.local.verify_tool(pc,tool_pin)
        driver=self.local.root/'pc-provider'
        if driver.exists():
            present={str(p.relative_to(self.local.root)).replace('\\','/') for p in driver.rglob('*') if p.is_file() or p.is_symlink()}
            e.require(present<=set(old['files'] if old else {}),'An unknown PC provider is already present; it was preserved.')
        if old:
            if old['bundle']!=bundle.pin:
                e.require(backup,'Updating a different checked package requires the confirmed recoverable workflow.')
                return self.update(bundle,pc,pair,address,port,old,tool)
            missing=[]
            for name,pin in old['files'].items():
                path=self.local.root/e.relative(name)
                if not path.exists():missing.append((path,planned[name]))
                else:e.require(e.sha(e.read(path))==pin['sha256'],'Changed provider files preserved.')
            if backup:
                helper=load_helper(e,'steam_link_setup_checkpoint');checkpoint=self.checkpoint(pair['pair_id'])
                checkpoint.begin('repair',[checkpoint.operation(path,[helper.file_value(raw)]) for path,raw in missing],[],bool(pc.get('registered')),target=bundle.pin)
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
        if backup:
            helper=load_helper(e,'steam_link_setup_checkpoint');checkpoint=self.checkpoint(pair['pair_id'])
            planned_ops=[checkpoint.operation(self.local.root/name,[helper.file_value(raw)]) for name,raw in planned.items()]
            planned_ops.extend([checkpoint.operation(key,[helper.file_value(pair_raw)]),checkpoint.operation(active,[helper.file_value(active_raw)])])
            # The receipt is deterministic and can be in either absent or fully
            # published state if a response is lost during installation.
            _,_,baseline=edits.planned()
            receipt={**version_fields(bundle.pc),'schema':RECEIPT,'bundle':bundle.pin,'files':{n:bundle.value['files'][n] for n in PC_FILES},
                     'settings':{'path':str(self.settings_path()),'baseline':baseline},'registered_before':bool(pc.get('registered')),
                     'pair':pair['pair_id'],'vrpathreg':tool_pin}
            planned_ops.append(checkpoint.operation(self.receipt,[helper.file_value(e.encoded(receipt))]))
            settings=[checkpoint.setting(self.settings_path(),SETTINGS_SECTION,k,[{'present':True,'value':v}]) for k,v in edits.edits[SETTINGS_SECTION].items()]
            checkpoint.begin('install',planned_ops,settings,bool(pc.get('registered')),target=bundle.pin)
        try:
            for name,raw in planned.items():
                path=self.local.root/e.relative(name);e.put(path,raw);undo.append((path,raw))
            settings=edits.apply()
            e.put(key,pair_raw);undo.append((key,pair_raw));e.put(active,active_raw);undo.append((active,active_raw))
            if not pc.get('registered'):
                self.local.register(tool,'adddriver');registered=True
                e.require(self.local.inventory()['registered'],'The PC provider registration could not be confirmed.')
            value={**version_fields(bundle.pc),'schema':RECEIPT,'bundle':bundle.pin,'files':{n:bundle.value['files'][n] for n in PC_FILES},
                   'settings':settings,'registered_before':bool(pc.get('registered')),'pair':pair['pair_id'],'vrpathreg':tool_pin}
            e.put(self.receipt,e.encoded(value))
        except BaseException:
            if not pc.get('registered'):
                self.local.register(tool,'removedriver')
            if settings:SettingsChange.restore(e,settings)
            for path,raw in reversed(undo):e.remove_owned(path,raw)
            raise
        return {'reason':'installed','changed':True,'automatic_restart':False}

    def update(self,bundle,pc,pair,address,port,old,tool):
        """Retain the initial uninstall baseline and checkpoint this update."""
        e=self.e;helper=load_helper(e,'steam_link_setup_checkpoint')
        e.require(not blocked_version_update(bundle.pc.get('version'),old.get('version')),
                  'The installed PC patch is newer or the package is unversioned; downgrade was not applied.')
        e.require(old['pair']==pair['pair_id'],'The existing connection pairing must be preserved during an update.')
        e.require(e.parse(e.read(self.local.root/'pairings/active.json',4096,private=True))==
                  {'schema':e.ACTIVE,'pair_id':old['pair']},'The active connection changed; it was preserved.')
        changes=[]
        for name,pin in old['files'].items():
            path=self.local.root/e.relative(name);raw=e.read(path) if path.exists() else None
            e.require(raw is None or len(raw)==pin['bytes'] and e.sha(raw)==pin['sha256'],
                      'Changed provider files preserved.')
            desired=bundle.files[name]
            if raw!=desired:changes.append((path,raw,desired))
        settings_path=self.settings_path();before=e.read(settings_path,2*1024*1024)
        preferences=e.parse(before)
        replacement=e.parse(e.encoded(old))
        desired={SETTINGS_SECTION:{'enableStreamBridge':True,'controlListenAddress':private_ipv4(address),'controlPort':port}}
        for section,spec in old['settings']['baseline'].items():
            current=preferences.get(section)
            e.require(type(current)is dict and all(key in current and type(current[key])is type(entry['installed']) and
                      current[key]==entry['installed'] for key,entry in spec['keys'].items()),'Changed SteamVR settings preserved; update stopped.')
            for key in spec['keys']:
                current[key]=desired[section][key]
                replacement['settings']['baseline'][section]['keys'][key]['installed']=current[key]
        replacement.update(bundle=bundle.pin,files={n:bundle.value['files'][n] for n in PC_FILES},
                           vrpathreg=bundle.pc['runtime_files']['vrpathreg'])
        replacement.pop('version',None);replacement.update(version_fields(bundle.pc))
        receipt_before=e.read(self.receipt,2*1024*1024,private=True);receipt_after=e.encoded(replacement)
        checkpoint=self.checkpoint(pair['pair_id'])
        operations=[checkpoint.operation(path,[helper.file_value(raw)]) for path,_,raw in changes]
        operations.append(checkpoint.operation(self.receipt,[helper.file_value(receipt_after)]))
        settings=[checkpoint.setting(settings_path,section,key,[{'present':True,'value':value}])
                  for section,values in desired.items() for key,value in values.items()]
        checkpoint.begin('update',operations,settings,bool(pc.get('registered')),target=bundle.pin)
        for path,previous,raw in changes:e.put(path,raw,previous)
        mode=stat.S_IMODE(settings_path.stat().st_mode)
        e.put(settings_path,e.encoded(preferences),before)
        if os.name!='nt':os.chmod(settings_path,mode)
        e.put(self.receipt,receipt_after,receipt_before)
        if not pc.get('registered'):
            self.local.register(tool,'adddriver')
            e.require(self.local.inventory()['registered'],'The updated provider registration could not be confirmed.')
        return {'reason':'updated','changed':True,'automatic_restart':False}

    def uninstall(self,pc,*,backup=False,target=None):
        e=self.e;state=self.state();checkpoint=self.checkpoint();pending=checkpoint.value()
        if state is None and pending:
            for operation in pending['operations']:
                if operation['path']!=str(self.receipt):continue
                for item in reversed([operation['before'],*operation['after']]):
                    if item and item['kind']=='file':
                        state=self.validate_state(e.parse(base64.b64decode(item['bytes'],validate=True)));break
                if state:break
        if state is None:
            e.require(not pc.get('registered') and not any(os.path.lexists(self.local.root/name) for name in PC_FILES),
                      'The PC removal receipt is missing; existing provider files or registration were preserved.')
            return {'reason':'not_installed','automatic_restart':False}
        e.require(not self.local.runner.runtime_running(),'Close SteamVR on this PC before removing the connection.')
        helper=load_helper(e,'steam_link_setup_checkpoint')
        known={item['path']:item for item in pending['operations']} if pending else {}
        active=self.local.root/'pairings/active.json'
        active_before=e.read(active,4096,private=True) if active.exists() else None
        if active_before is not None:
            prior=known.get(str(active));observed=helper.read_value(e,active)
            e.require(active_before==e.encoded({'schema':e.ACTIVE,'pair_id':state['pair']}) or
                      prior and observed in [prior['before'],*prior['after']],
                      'Changed active pairing preserved; removal stopped.')
        receipt_before=e.read(self.receipt,2*1024*1024,private=True) if self.receipt.exists() else None
        e.require(receipt_before is None or e.parse(receipt_before)==state,'Changed removal receipt preserved.')
        for name,pin in state['files'].items():
            path=self.local.root/e.relative(name)
            if not os.path.lexists(path):continue
            current=helper.read_value(e,path);item=known.get(str(path))
            e.require(e.sha(e.read(path))==pin['sha256'] or item and current in [item['before'],*item['after']],
                      'Changed provider files preserved.')
        tool=self.local.verify_tool(pc,state['vrpathreg'])
        # Validate conditional settings restoration before unregistering anything.
        raw=e.read(Path(state['settings']['path']),2*1024*1024);current=e.parse(raw)
        for section,spec in state['settings']['baseline'].items():
            values=current.get(section,{})
            e.require(type(values)is dict,'Changed SteamVR settings preserved; removal stopped.')
            for key,entry in spec['keys'].items():
                observed={'present':key in values,'value':values.get(key)}
                choices=[{'present':True,'value':entry['installed']},{'present':entry['present'],'value':entry['value']}]
                if pending:
                    prior=next((item for item in pending['settings'] if item['path']==state['settings']['path'] and
                                item['section']==section and item['key']==key),None)
                    if prior:choices.extend([prior['before'],*prior['after']])
                e.require(any(e.encoded(observed)==e.encoded(choice) for choice in choices),
                          'Changed SteamVR settings preserved; removal stopped.')
                if entry['present']:current.setdefault(section,{})[key]=entry['value']
                elif section in current:current[section].pop(key,None)
            if section in current and not current[section] and not spec['present']:current.pop(section)
        if backup:
            helper=load_helper(e,'steam_link_setup_checkpoint');checkpoint=self.checkpoint(state['pair'])
            paths=[self.local.root/name for name in state['files']]+[self.local.root/'pairings/active.json',self.receipt]
            operations=[checkpoint.operation(path,[None]) for path in paths]
            settings=[checkpoint.setting(self.settings_path(),section,key,[{'present':entry['present'],'value':entry['value']}])
                      for section,spec in state['settings']['baseline'].items() for key,entry in spec['keys'].items()]
            checkpoint.begin('uninstall',operations,settings,bool(pc.get('registered')),target=target)
        if pc.get('registered'):self.local.register(tool,'removedriver')
        e.require(not self.local.inventory().get('registered'),'The owned PC driver registration could not be removed.')
        path=Path(state['settings']['path']);mode=stat.S_IMODE(path.stat().st_mode)
        if e.encoded(current)!=raw:e.put(path,e.encoded(current),raw)
        if os.name!='nt':os.chmod(path,mode)
        for name in state['files']:
            path=self.local.root/e.relative(name)
            if os.path.lexists(path):e.remove_owned(path,e.read(path))
        # Only this session's exact active pointer is removed; keep old user keys.
        if active_before is not None:e.remove_owned(active,active_before)
        if receipt_before is not None:e.remove_owned(self.receipt,receipt_before)
        return {'reason':'uninstalled','automatic_restart':False}


class FrameTransaction:
    """Durable exact-predecessor journal; no service start/stop/restart command."""
    def __init__(self, engine, home, root, settings_path):
        self.e,self.home,self.root=engine,Path(home),Path(root)
        self.settings_path=settings_path
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
        return self.validate_state(self.e.parse(self.e.read(self.path,64*1024*1024,private=True)))

    def validate_state(self,v):
        e=self.e
        e.require(type(v)is dict and set(v)-{'version'}=={'schema','home','root','bundle','phase','operations','settings','pair'} and
                  v['schema']==RECEIPT and v['home']==str(self.home) and v['root']==str(self.root) and
                  v['phase'] in ('pending','installed','removing') and re.fullmatch('[0-9a-f]{64}',v['bundle']) and
                  re.fullmatch('[0-9a-f]{32}',v['pair']) and type(v['operations'])is list and
                  len(v['operations'])<=96,'Unknown Frame installation journal preserved.')
        version_fields(v)
        allowed={self.home/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf',
                 self.home/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json',
                 self.home/'.config/SteamFrameNativePassthrough/control.json',
                 Path(os.environ.get('XDG_CONFIG_HOME') or self.home/'.config')/'vulkan/implicit_layer.d'/MANIFEST,
                 Path(os.environ.get('XDG_CONFIG_HOME') or self.home/'.config')/'systemd/user/steam.service.d/85-steam-link-native-passthrough.conf',
                 Path(os.environ.get('XDG_CONFIG_HOME') or self.home/'.config')/'systemd/user/steamvr.service.d'/CAMERA_HOOK}
        allowed_settings={self.settings_path(),self.home/'.local/share/Steam/config/steamvr.vrsettings'}
        e.require(Path(v['settings'].get('path','')) in allowed_settings,
                  'The recorded camera settings path is not owned by this installation.')
        validate_settings_receipt(e,v['settings'],Path(v['settings']['path']),'camera',('forceSceneAlphaBlending',))
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

    def begin(self,bundle,operations,settings,pair,*,version=None):
        e=self.e;e.require(self.state() is None,'A previous Frame connection must be repaired or removed first.')
        value={'schema':RECEIPT,'home':str(self.home),'root':str(self.root),'bundle':bundle,
               'phase':'pending','operations':operations,'settings':settings,'pair':pair}
        if version is not None:value['version']=patch_version(version)
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

    def rollback(self,*,skip_paths=()):
        state=self.state()
        if state is None:return
        self.e.require(type(skip_paths) in (tuple,list,set,frozenset) and
                       all(type(path)is str for path in skip_paths) and
                       set(skip_paths)<={op['path'] for op in state['operations']},
                       'Unknown transaction removal scope preserved.')
        for operation in reversed(state['operations']):
            if operation['path'] not in skip_paths:self.replace(operation,reverse=True)
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
        filename='native_fragment_stream_coverage.py';name='sfnp_install_'+filename[:-3]
        spec=importlib.util.spec_from_file_location(name,stage/'tools'/filename)
        module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
        coverage,_=module.transform(original)
        outputs={'coverage.spv':coverage}
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
        report={'schema':'sfnp-install-startup-health-v1','software_ready':result.get('status')=='ready',
                'headset_picture_verified':False,'status':result.get('status'),
                'reason':str(result.get('reason') or '')[:512],
                'process_identities':result.get('process_identities',{}),
                'automatic_restart':False,'automatic_rollback':False}
        # The camera can fail while Steam's interface remains usable. Observe
        # that independently; missing journal permission cannot fail setup or
        # authorize another restart. The headset picture still needs a verdict.
        camera=getattr(source,'camera_health',None)
        if callable(camera):
            try:observation=camera(receipt['baseline'])
            except Exception:observation=None
            if type(observation)is dict:report['camera_health']=observation
            else:report['camera_health']={'schema':'frame-camera-startup-observation-v1',
                'status':'unavailable','camera_pixels_verified':False,
                'automatic_restart':False,'automatic_rollback':False}
        return report


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
        self.tx=FrameTransaction(engine,self.home,self.root,self.settings_path)
        self.run=run or engine.Runner().run;self.machine=machine

    def camera_helper(self):
        # Remote requests provide the current manifest-verified helper. Never
        # execute a stale helper merely because its old receipt is intact.
        source=getattr(self,'_camera_source',None)
        if source is None:
            source=local_camera_source()
        return load_module(source,'sfnp_camera_'+self.e.sha(source)[:16])

    def settings_path(self):
        try:path=self.camera_helper().resolve_settings(home=self.home)
        except Exception as error:
            raise self.e.SetupError('The active SteamVR settings directory could not be verified. No camera preference was changed.') from error
        return self.tx.owned(path)

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

    def checkpoint(self):
        helper=load_helper(self.e,'steam_link_setup_checkpoint')
        legacy_helper=load_helper(self.e,'steam_link_setup_legacy')
        external={self.settings_path(),self.home/'.local/share/Steam/config/steamvr.vrsettings',
                  self.home/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf',
                  self.home/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json',
                  self.home/'.config/SteamFrameNativePassthrough/control.json',
                  self.config/'vulkan/implicit_layer.d'/MANIFEST,
                  self.config/'systemd/user/steam.service.d/85-steam-link-native-passthrough.conf',
                  self.config/'systemd/user/steamvr.service.d'/CAMERA_HOOK}
        names={LIBRARY,'libsfnp_stream_launch.so','coverage.spv','receiver.conf','launch.conf',
               'receiver.enable','pairings/active.json','install.json',CAMERA_CONFIG,'camera-session.pause'}|{n.removeprefix('frame/') for n in TOOLS|PROFILES}|RETIRED_FRAME_FILES
        def allowed(path):
            if path in external:return True
            if legacy_helper.allowed(self.home,path):return True
            if not path.is_relative_to(self.root):return False
            name=path.relative_to(self.root).as_posix()
            return name in names or re.fullmatch('pairings/[0-9a-f]{32}[.]json',name) is not None
        def current(path):self.tx.owned(path);return helper.read_value(self.e,path)
        def replace(path,old,new):self.tx.owned(path);return helper.replace_value(self.e,path,old,new)
        owner=self
        class FrameCheckpoint(helper.Checkpoint):
            def restore(checkpoint):
                value=checkpoint.value()
                if value is None:return super().restore()
                owner.camera_checkpoint_preflight(checkpoint,value)
                with owner.camera_context(checkpoint=value) as camera:
                    if camera:
                        owner.camera_call(camera,'restore_owned')
                        for item in value['settings']:
                            if item['section']=='camera' and item['key']=='forceSceneAlphaBlending':
                                desired=item['before']['value'] if item['before']['present'] else None
                                owner.e.require(desired is None or type(desired)is bool,
                                                'Unknown original camera checkpoint preserved.')
                                before=owner.camera_value(Path(item['path']))
                                owner.e.require({'present':before is not None,'value':before} in
                                    [item['before'],*item['after']], 'Changed camera checkpoint preserved.')
                                owner.camera_call(camera,'apply_owned_override',{
                                    'schema':'sfnp-camera-override-authority-v1','settings':item['path'],
                                    'before':before,'desired':desired,'section_present':item['section_present']})
                    return super().restore()
        return FrameCheckpoint(self.e,self.root/'setup-restore.json',allowed,current,replace)

    def backup(self,action,operations,settings,transaction_values,*,pause_settings=None,target=None,camera_override=None):
        e=self.e;helper=load_helper(e,'steam_link_setup_checkpoint');checkpoint=self.checkpoint()
        # A pause is mutable helper state, not an uninstall payload. Journal its
        # exact predecessor so Undo can restore an old target after config
        # reversal, including a cancellation before new config publication.
        pause_path=self.root/'camera-session.pause'
        pause={'schema':'sfnp-camera-session-pause-v1','settings':(pause_settings or settings)['path']}
        entries=[checkpoint.operation(pause_path,[helper.file_value(self.camera_helper().encoded(pause)),None])]
        for op in operations:
            after=op['after']
            if after and after['kind']=='file':after={**after,'mode':op['mode']}
            choices=[after]
            # A partial prior Uninstall may have removed the camera config.
            # Native quiescence can briefly stage only its exact saved config;
            # reserve that intermediate before the helper makes any write.
            if op['path']==str(self.root/CAMERA_CONFIG):
                for candidate in op.get('camera_intermediates',[]):
                    if candidate and candidate['kind']=='file':candidate={**candidate,'mode':op['mode']}
                    if candidate not in choices:choices.append(candidate)
            entries.append(checkpoint.operation(Path(op['path']),choices))
        entries.append(checkpoint.operation(self.tx.path,transaction_values))
        entries_settings=[] if settings.get('policy')==SESSION_POLICY else [checkpoint.setting(Path(settings['path']),section,key,[{'present':entry['present'],'value':entry['value']}]
                       if action=='uninstall' else [{'present':True,'value':entry['installed']}])
                       for section,spec in settings['baseline'].items() for key,entry in spec['keys'].items()]
        if action=='update' and settings.get('policy')!=SESSION_POLICY:
            for item in entries_settings:
                if item['section']=='camera' and item['key']=='forceSceneAlphaBlending':
                    original=settings['baseline']['camera']['keys']['forceSceneAlphaBlending']
                    item['after'].append({'present':original['present'],'value':original['value']})
        if camera_override is not None:
            desired=camera_override['desired'];before=camera_override['before']
            item=checkpoint.setting(Path(camera_override['settings']),'camera','forceSceneAlphaBlending',
                                    [{'present':desired is not None,'value':desired}])
            e.require(item['before']=={'present':before is not None,'value':before},
                      'The old camera preference changed before uninstall could be prepared.')
            entries_settings=[entry for entry in entries_settings if
                (entry['path'],entry['section'],entry['key'])!=(item['path'],item['section'],item['key'])]+[item]
        checkpoint.begin(action,entries,entries_settings,target=target)

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
        return {**version_status(state,bundle.frame),'architecture':self.machine(),'runtime_compatible':compatible,
                'runtime_versions':self.e.runtime_versions(self.steamvr),
                'installed':state is not None,'files_current':current,
                'recovery_pending':(self.root/'setup-restore.json').exists() or (self.root/'lifecycle.json').exists(),
                'legacy_installed':(self.home/'.config/SteamFrameNativePassthrough/install.json').exists(),
                'recovery_path':str(self.root/'setup-restore.json'),
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

    def camera_checkpoint_preflight(self,checkpoint,value):
        """Prove exact reversal scope before any live native settings mutation."""
        e=self.e
        for op in value['operations']:
            observed=checkpoint.current(Path(op['path']))
            e.require(observed==op['before'] or observed in op['after'],
                      'A changed file was preserved; camera recovery did not start.')
        for item in value['settings']:
            path=Path(item['path']);prefs=e.parse(e.read(path,2*1024*1024)) if path.exists() else {}
            section=prefs.get(item['section'],{})
            e.require(type(section)is dict,'Changed camera checkpoint preserved.')
            observed={'present':item['key'] in section,'value':section.get(item['key'])}
            e.require(e.encoded(observed) in [e.encoded(entry) for entry in [item['before'],*item['after']]],
                      'Changed camera checkpoint preserved.')

    def camera_value(self,path):
        prefs=self.e.parse(self.e.read(path,2*1024*1024)) if path.exists() else {}
        section=prefs.get('camera',{})
        self.e.require(type(section)is dict and ('forceSceneAlphaBlending' not in section or
                       type(section['forceSceneAlphaBlending'])is bool), 'Unknown current camera preference preserved.')
        return section.get('forceSceneAlphaBlending')

    def camera_state(self,checkpoint=None):
        state=self.tx.state();fallback=state
        if state and any(op['path']==str(self.root/CAMERA_CONFIG) for op in state['operations']):return state
        if checkpoint:
            for op in checkpoint['operations']:
                if op['path']!=str(self.tx.path):continue
                for entry in reversed([op['before'],*op['after']]):
                    if entry is None or entry['kind']!='file':continue
                    candidate=self.tx.validate_state(self.e.parse(base64.b64decode(entry['bytes'],validate=True)))
                    if any(o['path']==str(self.root/CAMERA_CONFIG) for o in candidate['operations']):return candidate
                    if fallback is None:fallback=candidate
        return fallback

    @contextmanager
    def camera_context(self,*,state=None,checkpoint=None):
        """Hold exact reviewed helper code in memory across payload deletion."""
        e=self.e;state=state or self.camera_state(checkpoint)
        operations={op['path']:op for op in state['operations']} if state else {}
        config_op=operations.get(str(self.root/CAMERA_CONFIG));source_op=operations.get(str(self.root/CAMERA_TOOL))
        if config_op and source_op:
            def admitted(path,observed):
                op=operations[str(path)]
                if observed in (op['before'],op['after']):return True
                if checkpoint:
                    item=next((item for item in checkpoint['operations'] if item['path']==str(path)),None)
                    if item:return observed==item['before'] or observed in item['after']
                return False
            for name,op in ((CAMERA_CONFIG,config_op),(CAMERA_TOOL,source_op)):
                path=self.root/name;observed=self.tx.current(path)
                # Checkpoint file metadata includes mode; transaction values do not.
                checkpoint_current=load_helper(e,'steam_link_setup_checkpoint').read_value(e,path)
                e.require(admitted(path,observed) or admitted(path,checkpoint_current),
                          'Changed camera helper or configuration preserved.')
                e.require(op['after'] is not None and op['after']['kind']=='file',
                          'Missing owned camera helper authority.')
                if name==CAMERA_CONFIG and observed is None:
                    # A not-yet-published initial operation can safely be staged
                    # solely to quiesce native state before its own reversal.
                    e.put(path,base64.b64decode(op['after']['bytes'],validate=True))
                    os.chmod(path,0o600)
            module=self.camera_helper()
            yield module,self.root
            return
        payload=getattr(self,'_camera_payload',None)
        if state is None and checkpoint:
            owned=[item for item in checkpoint['settings'] if
                   item['section']=='camera' and item['key']=='forceSceneAlphaBlending']
            if owned:
                e.require(len(owned)==1,'Ambiguous original camera checkpoint preserved.')
                # A legacy-only Uninstall has no paired install.json. Its exact
                # checkpoint grants this key through the current pinned API,
                # rather than rewriting an active native preference offline.
                state={'operations':[],'settings':{'path':owned[0]['path'],'policy':SESSION_POLICY}}
        if state is None:
            yield None;return
        e.require(payload is not None,'A verified camera helper is required to restore this older installation safely.')
        source,library=payload
        settings=state['settings']
        config=self.camera_config(None,settings,previous=state,library=library)
        # An old installation has no live session helper. Its exact receipt
        # grants a narrowly scoped native restore using this reviewed tool.
        self.root.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
        with tempfile.TemporaryDirectory(prefix='camera-restore-',dir=self.root.parent) as temporary:
            root=Path(temporary);os.chmod(root,0o700)
            e.put(root/CAMERA_CONFIG,config)
            module=load_module(source,'sfnp_camera_'+e.sha(source)[:16])
            yield module,root

    def prepare_camera_target(self,settings):
        pause=self.root/'camera-session.pause'
        if not os.path.lexists(pause):return
        value=self.e.parse(self.e.read(pause,16384,private=True))
        if value.get('settings')==settings['path']:return
        module=self.camera_helper()
        self.e.require(callable(getattr(module,'prepare_target',None)),
                       'The camera settings migration helper is unavailable; existing files were preserved.')
        module.prepare_target(self.root,Path(settings['path']),home=self.home)

    def camera_call(self,camera,method,*args):
        module,root=camera
        callback=getattr(module,method,None)
        self.e.require(callable(callback),'The reviewed camera helper API is unavailable; existing files were preserved.')
        active=self.settings_path()
        config=module.load_config(root)[0];configured=Path(config['settings'])
        if method=='apply_owned_override' and Path(args[0]['settings'])!=configured:
            # After config publication, Undo can still own a delta on the old
            # unused file. Its checkpoint is authority for THAT file only;
            # never pass its baseline to the active native API or replace a
            # live helper's config merely to make the paths agree.
            old=self.home/'.local/share/Steam/config/steamvr.vrsettings'
            self.e.require(configured==active and Path(args[0]['settings'])==old and old!=active,
                'The camera checkpoint refers to an unknown configuration; existing files were preserved.')
            with tempfile.TemporaryDirectory(prefix='camera-checkpoint-',dir=self.root.parent) as temporary:
                temporary=Path(temporary);os.chmod(temporary,0o700)
                self.e.put(temporary/CAMERA_CONFIG,self.e.encoded({
                    'schema':'sfnp-camera-session-config-v1','settings':str(old),'library':config['library']}))
                return self.camera_call((module,temporary),method,*args)
        if configured!=active:
            self.e.require(configured==self.home/'.local/share/Steam/config/steamvr.vrsettings',
                'The camera configuration changed to an unknown directory; existing files were preserved.')
            if method=='restore_owned':method='restore_inactive';args=(active,)
            elif method=='apply_owned_override':method='apply_owned_inactive';args=(*args,active)
            callback=getattr(module,method,None)
            self.e.require(callable(callback),'The reviewed inactive camera cleanup API is unavailable; existing files were preserved.')
        try:
            result=callback(root,*args,home=self.home) if method in ('restore_inactive','apply_owned_inactive') else callback(root,*args)
        except Exception as error:
            # Only exact authored helper failures may cross the SSH boundary.
            # Never serialize arbitrary exception text, paths or private state.
            authored=fixed_checks(getattr(self,'_camera_source',None) or
                local_camera_source())
            reason='check_'+hashlib.sha256(str(error).encode()).hexdigest()[:16]
            if isinstance(error,module.SafetyError) and reason in authored:
                raise self.e.SetupError(authored[reason]) from None
            raise self.e.SetupError('The native camera preference could not be restored safely. Existing files and recovery records were kept.') from error
        self.e.require(type(result)is dict and result.get('persisted') is True and
                       result.get('status') in ('restored','unchanged'),
                       'The native camera restoration was not confirmed; existing files were preserved.')
        return result

    def camera_config(self,bundle,settings,*,previous=None,library=None):
        """Pinned native API and one stable, explicitly granted legacy migration."""
        e=self.e
        if library is None:
            pin=bundle.frame['runtime_files']['openvr']
            base={'steamvr':self.steamvr,'home':self.home,'system':Path('/')}[pin['base']]
            library={'path':str(base/e.relative(pin['path'])),'bytes':pin['bytes'],'sha256':pin['sha256']}
        value={'schema':'sfnp-camera-session-config-v1','settings':settings['path'],'library':library}
        if previous is not None:
            old_config=next((op['after'] for op in previous['operations'] if op['path']==str(self.root/CAMERA_CONFIG)),None)
            if previous['settings'].get('policy')==SESSION_POLICY and old_config is not None:
                e.require(old_config.get('kind')=='file','Unknown camera session configuration preserved.')
                prior=e.parse(base64.b64decode(old_config['bytes'],validate=True))
                if 'legacy_migration' in prior:value['legacy_migration']=prior['legacy_migration']
            elif previous['settings'].get('policy')!=SESSION_POLICY:
                path=Path(settings['path']);prefs=e.parse(e.read(path,2*1024*1024)) if path.exists() else {}
                current=prefs.get('camera',{})
                if type(current)is dict and current.get('forceSceneAlphaBlending') is True:
                    section=settings['baseline']['camera'];entry=section['keys']['forceSceneAlphaBlending']
                    e.require((not entry['present'] and entry['value'] is None) or type(entry['value'])is bool,
                              'Unknown original camera preference preserved; migration was not applied.')
                    value['legacy_migration']={'id':secrets.token_hex(32),'section_present':section['present'],
                        'present':entry['present'],'value':entry['value'],'installed':True}
        return e.encoded(value)

    def camera_dropin(self):
        # Valve already starts vrstartup in ExecStartPre. Conditions precede
        # every pre-start command, including the vendor list we must preserve.
        return ('[Service]\nExecCondition=-/usr/bin/python3 -I -B '+self.systemd(str(self.root/CAMERA_TOOL))+
                ' startup --root '+self.systemd(str(self.root))+'\n').encode()

    def plan(self,bundle,pair,address,port,*,live=False):
        e=self.e;bundle.admit('Windows')
        e.require(self.machine().lower() in ('aarch64','arm64'),'The selected device is not a supported Steam Frame.')
        good,original=self.runtime(bundle);e.require(good and original is not None,'The Frame runtime does not match this package.')
        if not live:self.cold()
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
        settings=self.settings_path()
        change=SettingsChange(e,settings,{'camera':{'forceSceneAlphaBlending':True}})
        before,_,baseline=change.planned()
        # Keep the original key-level history, but own no persistent True.
        # The separate helper owns short-lived native settings leases instead.
        receipt={'path':str(settings),'baseline':baseline,'policy':SESSION_POLICY}
        operations.append(self.tx.operation(self.root/CAMERA_CONFIG,self.camera_config(bundle,receipt)))
        operations.append(self.tx.operation(self.config/'systemd/user/steamvr.service.d'/CAMERA_HOOK,self.camera_dropin()))
        return operations,receipt,(before,before)

    def install(self,bundle,pair,address,port,*,backup=False,live=False):
        e=self.e;current=self.tx.state()
        e.require(not blocked_version_update(bundle.frame.get('version'),current.get('version') if current else None),
                  'The installed Frame patch is newer or the package is unversioned; downgrade was not applied.')
        if current:
            e.require(current['phase']=='installed' and current['pair']==pair['pair_id'],
                      'Repair or remove the previous Frame transaction before replacing it.')
            if current['bundle']!=bundle.pin:
                e.require(backup and live,'Updating a different Frame package requires the confirmed recoverable workflow.')
                return self.update(bundle,pair,address,port,current)
            if self.inventory(bundle)['files_current']:return {'reason':'current','automatic_restart':False}
            if not live:self.cold(steam=True)
            missing=[]
            for operation in current['operations']:
                observed=self.tx.current(Path(operation['path']))
                if observed==operation['after']:continue
                e.require(observed is None and operation['before'] is None and operation['after'] is not None,
                          'Changed Frame files were preserved; only missing owned components can be repaired.')
                missing.append(operation)
            if backup:
                helper=load_helper(e,'steam_link_setup_checkpoint')
                self.backup('repair',missing,current['settings'],[helper.read_value(e,self.tx.path)],target=bundle.pin)
            for operation in missing:self.tx.replace(operation)
            self.system('daemon-reload')
            return {'reason':'repaired','automatic_restart':False,'manual_restart_required':True}
        operations,settings,(before,after)=self.plan(bundle,pair,address,port,live=live)
        if backup:
            helper=load_helper(e,'steam_link_setup_checkpoint')
            transaction={**version_fields(bundle.frame),'schema':RECEIPT,'home':str(self.home),'root':str(self.root),'bundle':bundle.pin,
                         'phase':'pending','operations':operations,'settings':settings,'pair':pair['pair_id']}
            pending=helper.file_value(e.encoded(transaction));transaction['phase']='installed'
            self.backup('install',operations,settings,[pending,helper.file_value(e.encoded(transaction)),None],target=bundle.pin)
        self.prepare_camera_target(settings)
        self.tx.begin(bundle.pin,operations,settings,pair['pair_id'],version=bundle.frame.get('version'))
        try:
            settings_path=Path(settings['path']);mode=stat.S_IMODE(settings_path.stat().st_mode) if settings_path.exists() else 0o600
            if after!=before:
                e.put(settings_path,after,before)
                os.chmod(settings_path,mode)
            self.system('daemon-reload')
        except BaseException:
            self.undo(live=live)
            raise
        return {'reason':'prepared','pair_id':pair['pair_id'],'automatic_restart':False,
                'manual_restart_required':True,'coverage_operational':False}

    def update(self,bundle,pair,address,port,current):
        """Replace owned payloads live while retaining the initial predecessors."""
        e=self.e;bundle.admit('Windows');compatible,original=self.runtime(bundle)
        e.require(not blocked_version_update(bundle.frame.get('version'),current.get('version')),
                  'The installed Frame patch is newer or the package is unversioned; downgrade was not applied.')
        e.require(compatible and original is not None,'The Frame runtime does not match this package.')
        e.require(self.machine().lower() in ('aarch64','arm64'),'The selected device is not a supported Steam Frame.')
        e.require(type(port)is int and 1024<=port<=65535,'Invalid connection port.')
        address=private_ipv4(address)
        e.require(e.parse(e.read(self.root/'pairings'/ (current['pair']+'.json'),4096,private=True))==pair,
                  'The existing Frame pairing must be preserved during an update.')
        for op in current['operations']:
            observed=self.tx.current(Path(op['path']))
            e.require(observed==op['after'] or observed is None and op['before'] is None and
                      op['after'] is not None and op['after']['kind']=='file',
                      'Changed Frame files were preserved; only owned components can be updated.')
        # A retired receiver installed after the original bridge receipt must
        # not become an untracked second layer. Refuse before staging or writes;
        # the checked legacy removal path can revoke it independently first.
        old_config=self.home/'.config/SteamFrameNativePassthrough'
        retired_paths=(self.home/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf',
                       self.home/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json')
        e.require(not any(os.path.lexists(path) for path in retired_paths),
                  'A retired Frame receiver is active; remove it with checked maintenance before updating.')
        old_control=old_config/'control.json'
        if old_control.exists():
            value=e.parse(e.read(old_control,4096,private=True))
            e.require(value.get('schema')=='sfnp-user-control-v1' and value.get('enabled') is False,
                      'A retired Frame receiver is active; remove it with checked maintenance before updating.')
        # Retain initial key history without treating a player's current camera
        # preference as an immutable installed payload. Only an exact old True
        # can grant the one-time legacy restoration in the helper configuration.
        settings={**current['settings'],'policy':SESSION_POLICY}
        settings_moved=Path(settings['path'])!=self.settings_path()
        if settings_moved:
            # The old unused file grants no baseline for the active native file.
            path=self.settings_path()
            _,_,baseline=SettingsChange(e,path,{'camera':{'forceSceneAlphaBlending':True}}).planned()
            settings={'path':str(path),'baseline':baseline,'policy':SESSION_POLICY}
        with tempfile.TemporaryDirectory(prefix='prepare-update-',dir=self.root.parent) as stage:
            outputs=generate_shaders(e,bundle,original,Path(stage))
        wanted={self.root/LIBRARY:bundle.files['frame/'+LIBRARY],
                self.root/'libsfnp_stream_launch.so':bundle.files['frame/libsfnp_stream_launch.so'],
                self.root/'receiver.conf':MARKER+address.encode()+b'\n'+str(port).encode()+b'\n',
                self.root/CAMERA_CONFIG:self.camera_config(bundle,settings,previous=None if settings_moved else current),
                self.config/'systemd/user/steamvr.service.d'/CAMERA_HOOK:self.camera_dropin()}
        wanted.update({self.root/name:raw for name,raw in outputs.items()})
        wanted.update({self.root/Path(name).relative_to('frame'):bundle.files[name]
                       for name in TOOLS|PROFILES})
        operations=e.parse(e.encoded(current))['operations'];by_path={op['path']:op for op in operations};changes=[]
        for path,raw in wanted.items():
            observed=self.tx.current(path);desired={'kind':'file','bytes':base64.b64encode(raw).decode()}
            if str(path) in by_path:by_path[str(path)]['after']=desired
            else:
                op=self.tx.operation(path,raw);operations.append(op);by_path[str(path)]=op
            if observed!=desired:
                changes.append({'path':str(path),'before':observed,'after':desired,'mode':by_path[str(path)]['mode']})
        for op in operations:
            path=Path(op['path'])
            if path not in wanted:
                if path.is_relative_to(self.root) and path.relative_to(self.root).as_posix() in RETIRED_FRAME_FILES:
                    # The earlier exact-current comparison above proves that
                    # this is our unchanged old artifact. Preserve op['before']
                    # for uninstall and the checkpoint's full old state for Undo.
                    op['after']=None
                observed=self.tx.current(path)
                if observed!=op['after']:changes.append({**op,'before':observed})
        pending={**current,'bundle':bundle.pin,'operations':operations,'settings':settings,'phase':'pending'}
        pending.pop('version',None);pending.update(version_fields(bundle.frame))
        committed={**pending,'phase':'installed'};helper=load_helper(e,'steam_link_setup_checkpoint')
        previous=e.read(self.tx.path,64*1024*1024,private=True)
        self.backup('update',changes,current['settings'],[helper.file_value(e.encoded(pending)),helper.file_value(e.encoded(committed))],pause_settings=settings,target=bundle.pin)
        e.put(self.tx.path,e.encoded(pending),previous)
        # All cold work and the exact recovery journal precede live quiescence.
        # A generation/backup failure must not pause an otherwise valid session.
        if current['settings'].get('policy')==SESSION_POLICY or settings_moved:
            with self.camera_context(state=current) as camera:
                self.camera_call(camera,'restore_owned')
        self.prepare_camera_target(settings)
        for op in changes:self.tx.replace(op)
        if current['settings'].get('policy')!=SESSION_POLICY and not settings_moved:
            # The verified new config/helper now exist, but native VR has not
            # restarted yet. Restore its old owned global True through the API.
            with self.camera_context() as camera:self.camera_call(camera,'restore_owned')
        self.system('daemon-reload')
        return {'reason':'prepared-update','pair_id':current['pair'],'automatic_restart':False,
                'manual_restart_required':True,'coverage_operational':False}

    def undo(self,*,backup=False,live=False,purge_legacy=False,target=None):
        state=self.tx.state();legacy=None;legacy_helper=None;checkpoint=self.checkpoint();pending=checkpoint.value()
        if state is None and purge_legacy and pending:
            for operation in pending['operations']:
                if operation['path']!=str(self.tx.path):continue
                for item in reversed([operation['before'],*operation['after']]):
                    if item and item['kind']=='file':
                        state=self.tx.validate_state(self.e.parse(base64.b64decode(item['bytes'],validate=True)));break
                if state:break
        if purge_legacy:
            legacy_helper=load_helper(self.e,'steam_link_setup_legacy')
            if state and self.tx.state() is None:
                from types import SimpleNamespace
                tx=SimpleNamespace(state=lambda:state,owned=self.tx.owned,current=self.tx.current,operation=self.tx.operation)
                view=SimpleNamespace(e=self.e,home=self.home,tx=tx,settings_path=self.settings_path,camera_value=self.camera_value)
            else:view=self
            legacy=legacy_helper.plan(view,checkpoint=pending)
        if state is None and legacy is None:return {'reason':'not_installed','automatic_restart':False}
        if not live:self.cold(steam=True)
        legacy_paths={op['path'] for op in legacy['operations']} if legacy else set()
        known={item['path']:item for item in pending['operations']} if pending else {}
        operations={op['path']:{**op,'after':None if op['path'] in legacy_paths else op['before']}
                    for op in state['operations']} if state else {}
        if purge_legacy:
            for operation in operations.values():
                observed=self.tx.current(Path(operation['path']));prior=known.get(operation['path'])
                candidates=[operation['before'],operation['after']]
                if state:
                    original=next(item for item in state['operations'] if item['path']==operation['path'])
                    candidates.extend([original['before'],original['after']])
                    if operation['path']==str(self.root/CAMERA_CONFIG):operation['camera_intermediates']=[original['after']]
                if prior:candidates.extend(None if item is None else {k:v for k,v in item.items() if k!='mode'} for item in [prior['before'],*prior['after']])
                self.e.require(observed in candidates,'Changed Frame uninstall files were preserved before removal.')
                operation.update(before=observed,owned_intermediates=candidates)
        if legacy:operations.update({op['path']:op for op in legacy['operations']})
        if backup:
            settings=state['settings'] if state else {'path':str(self.settings_path()),'policy':SESSION_POLICY}
            self.backup('uninstall',list(operations.values()),settings,[None],camera_override=legacy['camera'] if legacy else None,target=target)
        if state:
            with self.camera_context(state=state,checkpoint=self.checkpoint().value() if purge_legacy else None) as camera:
                if camera:self.camera_call(camera,'restore_owned')
        if legacy:legacy_helper.apply_camera(self,legacy)
        # Revoke new activation first; never restore the old layer while the new
        # compositor is still using the same native descriptor path.
        marker=self.root/'receiver.enable'
        if state and marker.exists():
            expected=next(o['after'] for o in state['operations'] if o['path']==str(marker))
            self.e.require(self.tx.current(marker)==expected,'Changed enable marker preserved.')
            self.e.remove_owned(marker,MARKER)
        settings=state['settings'] if state else None
        try:
            if settings and not (legacy and legacy['camera']):SettingsChange.restore(self.e,settings)
        except self.e.SetupError:
            # Pending transaction may have failed before the settings write.
            current=self.e.parse(self.e.read(Path(settings['path']),2*1024*1024))
            for section,spec in settings['baseline'].items():
                values=current.get(section,{})
                self.e.require(all((key in values)==entry['present'] and
                    (not entry['present'] or values[key]==entry['value']) for key,entry in spec['keys'].items()),
                    'The camera preference changed; it was preserved and recovery remains available.')
        if purge_legacy:
            # Revoke/delete paired artifacts in reverse publication order, but
            # keep the retired remover's validated order: its receipt is last.
            removal=[operation for operation in reversed(list(operations.values())) if operation['path'] not in legacy_paths]
            if legacy:removal.extend(operations[operation['path']] for operation in legacy['operations'])
            for operation in removal:
                observed=self.tx.current(Path(operation['path']))
                self.e.require(observed in [operation['before'],operation['after'],*operation.get('owned_intermediates',[])],
                               'A Frame component changed during uninstall; it was preserved.')
                self.tx.replace({**operation,'before':observed})
            if self.tx.path.exists():self.e.remove_owned(self.tx.path,self.e.read(self.tx.path,64*1024*1024,private=True))
        else:
            if state:self.tx.rollback()
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
        e=engine;e.require(type(value)is dict and set(value)-{'version'}=={'schema','readiness','bridge_protocol','pc','frame','files'} and value['schema']==SCHEMA,'Unknown inventory manifest.')
        version_fields(value)
        e.protocol(value['bridge_protocol']);e.records(value['files'])
        self.pin=e.sha(e.encoded(value));self.frame=value['frame'];self.value=value
        e.require(type(self.frame)is dict and {'runtime_files','files','reviewed','generated'}==set(self.frame)-{'version'},'Unknown Frame inventory shape.')
        version_fields(self.frame)
        pins=self.frame['runtime_files'];e.require(type(pins)is dict and 1<=len(pins)<=32,'Bounded runtime inventory required.')
        for pin in pins.values():
            e.require(type(pin)is dict and set(pin)=={'base','path','bytes','sha256'} and pin['base'] in ('steamvr','home','system'),'Unknown inventory root.')
            e.relative(pin['path']);e.records({pin['path']:{k:pin[k] for k in ('bytes','sha256')}})

def remote_request(engine,request,*,unit=None):
    e=engine;unit=unit or FrameInstall(e)
    command=request['command']
    view=ReadOnlyView(e,request['manifest']);pin=view.value['files']['frame/'+CAMERA_TOOL]
    encoded=request.get('camera_source')
    e.require(type(encoded)is str,'The verified camera cleanup helper is missing.')
    source=base64.b64decode(encoded,validate=True)
    e.require(len(source)==pin['bytes'] and e.sha(source)==pin['sha256'],
              'The camera cleanup helper differs from the checked package.')
    unit._camera_source=source
    if command in ('bridge-inventory','bridge-health-baseline','bridge-health','bridge-runtime-restart'):
        view=ReadOnlyView(e,request['manifest'])
        if command=='bridge-inventory':return unit.inventory(view)
        tool=request.get('health_source');name='frame/tools/compositor_startup_health.py';pin=view.value['files'][name]
        e.require(type(tool)is str,'The read-only startup checker is missing.')
        source=base64.b64decode(tool,validate=True)
        e.require(len(source)==pin['bytes'] and e.sha(source)==pin['sha256'],'The startup checker changed.')
        observer=StartupHealth(load_module(source,'sfnp_install_health'),unit.home)
        if command=='bridge-health-baseline':return observer.baseline()
        if command=='bridge-health':return observer.check(request['baseline'])
    if command in ('bridge-runtime-snapshot','bridge-runtime-restart','bridge-runtime-restored'):
        lifecycle=load_helper(e,'steam_link_setup_frame').FrameLifecycle(e,unit.root)
        if command=='bridge-runtime-snapshot':
            identity=request.get('continuation')
            if identity is not None:
                value=unit.checkpoint().value();handoff=value.get('continuation') if value else None
                e.require(request.get('automated') is True and handoff and handoff['phase']=='ready' and
                          handoff['target']==view.pin and handoff['id']==identity,
                          'The Frame continuation does not match the checked recovery records.')
                return lifecycle.continue_prepared(identity)
            return lifecycle.snapshot()
        if command=='bridge-runtime-restart':return lifecycle.restart(restoring=request.get('restoring') is True,
            health_receipt=observer.baseline())
        return lifecycle.restored(health_ok=request.get('health_ok') is True)
    if command in ('bridge-restore','bridge-undo','bridge-prepare'):
        provider=view.frame['runtime_files']['openvr']
        base={'steamvr':unit.steamvr,'home':unit.home,'system':Path('/')}[provider['base']]
        path=base/e.relative(provider['path']);raw,_=e.read_public_reference(path,provider['bytes'])
        e.require(len(raw)==provider['bytes'] and e.sha(raw)==provider['sha256'],
                  'The native camera provider does not match the checked package.')
        unit._camera_payload=(source,{'path':str(path),'bytes':provider['bytes'],'sha256':provider['sha256']})
    if command=='bridge-restore':
        continuation=request.get('continuation',False)
        removing=request.get('removing',False)
        e.require(type(continuation)is bool and (not continuation or request.get('automated') is True),
                  'Setup continuation requires the confirmed Apply workflow.')
        e.require(type(removing)is bool and (not removing or continuation),
                  'Removal continuation requires the confirmed Uninstall workflow.')
        checkpoint=unit.checkpoint()
        if removing:
            # Transfer exact Undo authority without restoring or activating any
            # predecessor. Explicit Uninstall must proceed directly to vanilla.
            result={'continuation':checkpoint.retain_current(view.pin),'removing':True}
        else:
            result=checkpoint.restore()
            if continuation:result['continuation']=checkpoint.reconcile(view.pin)
            unit.system('daemon-reload')
        state=unit.tx.state()
        return {**result,'installed':state is not None,'installed_version':patch_version(state.get('version')) if state else None}
    if command=='bridge-checkpoint-finish':unit.checkpoint().finish(restored=request.get('restored') is True);return {'reason':'complete'}
    if command=='bridge-undo':return unit.undo(backup=request.get('automated') is True,live=request.get('automated') is True,
                                               purge_legacy=request.get('automated') is True,target=view.pin)
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
        if command=='bridge-prepare':return unit.install(bundle,request['pairing'],request['address'],request['port'],
            backup=request.get('automated') is True,live=request.get('automated') is True)
        raise e.SetupError('Unknown fixed Frame operation.')


class Remote:
    def __init__(self,engine,address,login='steamos',*,runner=None,interactive=False):
        self.e=engine;self.address=engine.host(address);self.login=engine.user(login)
        self.runner=runner or engine.Runner();self.interactive=interactive
        self.session=None;self.log=None
        self.auth=None;self.cancelled=None
        self.engine_source=Path(engine.__file__).read_bytes()
        self.bridge_source=Path(__file__).read_bytes()
        self.helper_sources={name:Path(__file__).with_name(name+'.py').read_bytes() for name in
                             ('steam_link_setup_checkpoint','steam_link_setup_frame','steam_link_setup_legacy')}
        self.camera_source=local_camera_source()
        self.checks=fixed_checks(self.engine_source,self.bridge_source,self.camera_source,*self.helper_sources.values())

    def close(self):
        if self.session is not None:self.session.close()

    @property
    def needs_terminal(self):
        """Only the first interactive SSH authentication borrows the console."""
        return self.interactive and getattr(self,'auth',None) is None and (self.session is None or self.session.closed)

    def call(self,request):
        e=self.e
        command=request.get('command')
        stage={'bridge-inventory':'frame_inventory','bridge-prepare':'frame_prepare',
               'bridge-commit':'frame_commit','bridge-undo':'undo',
               'bridge-health-baseline':'startup_baseline','bridge-health':'startup_health'}.get(command,'preflight')
        if self.log:self.log.event(stage,'stage',command=command,status='started')
        if self.session is None:
            # One authenticated SSH process for the entire wizard, including
            # Check/prepare/commit/health/Undo. OpenSSH keeps the connection;
            # we never read a password or create credentials/authorized_keys.
            source=('import base64,sys,types\n'
                    'e=types.ModuleType("sfnp_remote_engine");e.__file__="<verified-setup-engine>"\n'
                    'sys.modules[e.__name__]=e\n'
                    f'exec(compile(base64.b64decode({base64.b64encode(self.engine_source).decode()!r}),e.__file__,"exec"),e.__dict__)\n'
                    'b=types.ModuleType("sfnp_remote_bridge");b.__file__="<verified-bridge-engine>"\n'
                    'sys.modules[b.__name__]=b\n'
                    f'bridge_source=base64.b64decode({base64.b64encode(self.bridge_source).decode()!r})\n'
                    'exec(compile(bridge_source,b.__file__,"exec"),b.__dict__)\n'
                    f'helpers={ {name:base64.b64encode(raw).decode() for name,raw in self.helper_sources.items()}!r}\n'
                    'helper_sources=[]\n'
                    'for name,raw in helpers.items():\n'
                    ' source=base64.b64decode(raw);module=types.ModuleType("sfnp_"+name);module.__file__="<verified-helper>"\n'
                    ' sys.modules[module.__name__]=module;exec(compile(source,module.__file__,"exec"),module.__dict__)\n'
                    ' module._sfnp_verified_helper=True;helper_sources.append(source)\n'
                    f'checks=b.fixed_checks(base64.b64decode({base64.b64encode(self.engine_source).decode()!r}),bridge_source,base64.b64decode({base64.b64encode(self.camera_source).decode()!r}),*helper_sources)\n'
                    'def dispatch(request):\n'
                    ' try:return b.remote_request(e,request)\n'
                    ' except Exception as error:return {"error":True,"failure":b.failure_record(e,error,checks)}\n')
            session=load_helper(e,'steam_link_setup_session')
            self._session_errors=(session.SessionError,)
            auth=self.auth.open(self.address,self.login) if self.auth is not None and self.interactive else None
            self.session=session.Session(e.ssh_args(self.address,self.login,interactive=self.interactive,inline_auth=auth is not None),source,
                interactive=self.interactive,timeout=90,auth=auth,cancelled=self.cancelled)
        # Ordered Frame activation may wait for three native service phases.
        # Their individual 30-second bounds plus RPC overhead must fit inside
        # this request; other operations retain their existing timeout.
        try:result=self.session.call(request,timeout=120) if command=='bridge-runtime-restart' else self.session.call(request)
        except (OSError,ValueError,TimeoutError) as error:
            if self.log:self.log.record_failure(stage,error,command=command,
                reason=error.reason if isinstance(error,getattr(self,'_session_errors',())) else None)
            if isinstance(error,getattr(self,'_session_errors',())):
                failure=e.SetupError(str(error));failure._sfnp_public_connection_failure=True
                raise failure from None
            raise e.SetupError('The SSH connection ended or timed out. The result is unconfirmed. Choose Try again to check the recorded state before continuing.') from None
        if result.get('error'):
            failure=result.get('failure',{})
            if type(failure)is not dict:failure={}
            reason=failure.get('reason')
            if reason not in self.checks and reason not in ('setup_refused','os_error','invalid_data'):reason='setup_refused'
            logged_reason={'setup_refused':'remote_refused','os_error':'io_error','invalid_data':'invalid_input'}.get(reason,reason)
            if self.log:self.log.event(stage,'failure',command=command,reason=logged_reason,status='refused',errno=failure.get('errno'))
            message=self.checks.get(reason,'The Frame refused this operation. Check the setup log and recovery paths shown below.')
            number=failure.get('errno')
            if reason=='os_error' and type(number)is int and 0<number<4096:message+=' OS error '+str(number)+'.'
            failure=e.SetupError(message);failure._sfnp_public_connection_failure=True
            raise failure
        if self.log:self.log.event(stage,'result',command=command,status='ok')
        return result


def request(bundle,command,**values):
    result={'command':command,'manifest':bundle.value,**values}
    if command=='bridge-prepare':
        result['files']={name:{**bundle.value['files'][name],
            'data':base64.b64encode(raw).decode()} for name,raw in bundle.files.items()}
    result['camera_source']=base64.b64encode(bundle.files['frame/'+CAMERA_TOOL]).decode()
    if command in ('bridge-health-baseline','bridge-health','bridge-runtime-restart'):
        result['health_source']=base64.b64encode(bundle.files['frame/tools/compositor_startup_health.py']).decode()
    return result


def terminal_scope(engine,ui,remote):
    # An established OpenSSH session needs no terminal. Keeping normal RPCs in
    # the app preserves its progress/health/recovery display during long work.
    return ui.suspend() if ui and getattr(remote,'needs_terminal',True) else engine.nullcontext()


class Automation:
    """Production ports for the confirmed, recoverable setup workflow."""
    def __init__(self,engine,bundle,local,installed,remote,ui,context,factory,address,login):
        self.e,self.bundle,self.local,self.installed=engine,bundle,local,installed
        self.remote,self.ui,self.context=remote,ui,context
        self.factory,self.address,self.login=factory,address,login
        pc=local.inventory();self.pc=pc
        self.runtime=load_helper(engine,'steam_link_setup_pc').Lifecycle(engine,local,pc['steamvr'])
        self.error=engine.SetupError;self.baseline=None;self.restore_baseline=None

    def phase(self,stage):
        self.context['stage']=stage
        if hasattr(self.ui,'stage'):self.ui.stage(stage)
        log=self.context['log']
        if log:log.event(stage,'stage',status='started')

    def call(self,command,**values):
        # Reconnection is confined to a deliberate operation/recovery attempt.
        # It never blindly resends an uncertain installation request.
        if getattr(getattr(self.remote,'session',None),'closed',False):
            self.remote=self.factory(self.address,self.login,interactive=True)
        with terminal_scope(self.e,self.ui,self.remote):return self.remote.call(request(self.bundle,command,**values))

    def begin(self):
        self.pc=self.local.inventory();self.runtime.snapshot()
        self.phase('startup_baseline');self.baseline=self.call('bridge-health-baseline')
        self.call('bridge-runtime-snapshot')
        self.local.apply_started=True

    def stop_pc(self):
        self.phase('runtime_stop');self.runtime.stop()

    def prepare(self,action):
        if action=='uninstall':
            self.phase('undo');self.call('bridge-undo',automated=True)
            self.installed.uninstall(self.local.inventory(),backup=True,target=self.bundle.pin)
            return
        state=self.installed.state()
        pair=self.local.pairing(state['pair']) if state else {'schema':self.e.PAIR,'pair_id':secrets.token_hex(16),
              'key_hex':secrets.token_hex(32),'role':'producer','protocol_major':1}
        address=local_address(self.address)
        self.phase('frame_prepare')
        self.call('bridge-prepare',pairing={**pair,'role':'receiver'},address=address,port=47631,automated=True)
        self.phase('pc_install')
        self.installed.install(self.bundle,self.local.inventory(),pair,address,47631,backup=True)
        self.phase('frame_commit');self.call('bridge-commit')

    def restart_prepared(self):
        # The maintainer wants the PC runtime left closed after Apply. Steam
        # Link/game launch owns its later startup. Only rollback restores the
        # PC's original on/off state through runtime.restore().
        self.phase('runtime_start');result=self.call('bridge-runtime-restart')
        if type(result.get('health_receipt'))is dict:self.baseline=result['health_receipt']

    def prepared_health(self):
        self.phase('startup_health');result=self.call('bridge-health',baseline=self.baseline)
        return result

    def restore_files(self,*,continuation=False):
        self.phase('rollback')
        # A PC-side recovery error must never strand the Frame in a broken
        # startup. Restore/restart/check its UI independently, then report any
        # remaining PC recovery failure without claiming complete rollback.
        restored=self.call('bridge-restore',continuation=continuation,automated=True)
        self.restored_frame=restored
        self.pc_recovery_error=None
        try:
            self.runtime.snapshot();self.runtime.stop()
            checkpoint=self.installed.checkpoint();result=checkpoint.restore()
            if result['registered'] is not None:
                pc=self.local.inventory();desired=result['registered']
                if bool(pc.get('registered'))!=desired:
                    tool=self.local.verify_tool(pc,self.bundle.pc['runtime_files']['vrpathreg'])
                    self.local.register(tool,'adddriver' if desired else 'removedriver')
                self.e.require(bool(self.local.inventory().get('registered'))==desired,
                               'The original PC driver registration could not be restored.')
        except Exception as error:
            self.pc_recovery_error=error
            record_failure(self.context['log'],self.e,'rollback',error)

    def reconcile_for_apply(self):
        """Prepare a checked continuation without claiming completed recovery.

        Both exact original journals remain until new checkpoints atomically
        inherit their Undo predecessors. Original runtime baselines survive;
        only the eventual new package receives a restart and picture question.
        """
        if self.context.get('operation_action')=='uninstall':
            self.phase('preflight')
            self.runtime.snapshot();self.runtime.stop()
            self.restored_frame=self.call('bridge-restore',continuation=True,removing=True,automated=True)
            self.installed.checkpoint().retain_current(self.bundle.pin)
            self.pc_recovery_error=None
        else:
            self.restore_files(continuation=True)
            self.e.require(self.pc_recovery_error is None,
                           'The saved PC state could not be prepared for this update; recovery records were kept.')
            checkpoint=self.installed.checkpoint();checkpoint.reconcile(self.bundle.pin)
        self.phase('preflight')
        self.call('bridge-runtime-snapshot',continuation=self.restored_frame['continuation'],automated=True)

    def picture_observation(self,*,original,status):
        # One bounded event per question/result, with explicit intended picture
        # and package versions. Never log screenshots or arbitrary UI answers.
        self.context['stage']='picture_check'
        if hasattr(self.ui,'stage'):self.ui.stage('picture_check')
        log=self.context.get('log')
        if not log:return
        pc=version_status(self.installed.state(),self.bundle.pc)
        restored=getattr(self,'restored_frame',{})
        removed=self.context.get('operation_action')=='uninstall'
        frame={'installed':restored.get('installed') if original else not removed,
               'installed_version':restored.get('installed_version') if original else None if removed else patch_version(self.bundle.frame.get('version')),
               'target_version':patch_version(self.bundle.frame.get('version'))}
        action='rollback' if original else self.context.get('operation_action','install')
        log.event('picture_check','stage' if status=='started' else 'result',status=status,
                  action=action,versions={'pc':pc,'frame':frame})

    def restart_original(self):
        self.phase('runtime_start')
        if self.restore_baseline is None:self.restore_baseline=self.call('bridge-health-baseline')
        result=self.call('bridge-runtime-restart',restoring=True)
        # The durable activation owns the pre-restart observation. Reusing it
        # after a lost reply/new RPC must not wait for another fresh invocation.
        if type(result.get('health_receipt'))is dict:self.restore_baseline=result['health_receipt']
        if self.pc_recovery_error is None:
            try:self.runtime.restore()
            except Exception as error:
                self.pc_recovery_error=error
                record_failure(self.context['log'],self.e,'rollback',error)

    def original_health(self):
        self.phase('startup_health');result=self.call('bridge-health',baseline=self.restore_baseline)
        # A usable Frame does not complete a failed PC restoration. Resolve
        # that operation before asking for Yes, not during record cleanup.
        self.e.require(self.pc_recovery_error is None,
                       'The original Frame interface is running, but PC recovery is incomplete. Retry can continue recovery.')
        return result

    def finish(self,*,restored):
        # Never discard original-state authority before the Frame UI passed
        # health and its two actual service instances are freshly running.
        if restored:self.e.require(self.pc_recovery_error is None,
                                  'The original Frame interface is running, but PC recovery is incomplete. Retry can continue recovery.')
        self.call('bridge-runtime-restored',health_ok=True)
        self.call('bridge-checkpoint-finish',restored=restored)
        self.installed.checkpoint().finish(restored=restored);self.runtime.finish()
        if not restored:self.e.remember_frame(self.local,self.address,self.login)

    def failure(self,error,*,recovery):
        record_failure(self.context['log'],self.e,self.context['stage'],error)
        support_paths(self.local,self.context)
        if isinstance(error,self.e.SetupError):print(str(error),file=sys.stderr)


def support_paths(local,context):
    """Show durable evidence before Undo prompts and again on terminal failures."""
    if context.get('paths_printed'):return
    ui=context.get('ui');log=context.get('log')
    def show(key,value):
        if ui:ui.path(value,key=key)
        else:
            labels={'setup_log':'Setup log: ','setup_log_unavailable':'Setup log could not be created: ',
                    'recovery_pc':'PC recovery record (if created): ',
                    'recovery_frame':'Frame recovery record (if created): '}
            print(labels[key]+str(value),file=sys.stderr)
    if log and log.path:show('setup_log' if log.available else 'setup_log_unavailable',log.path)
    show('recovery_pc',local.root/'setup-restore.json')
    show('recovery_pc',local.root/'lifecycle.json')
    show('recovery_frame',context.get('frame_recovery') or '$HOME/.local/share/SteamLinkNativePassthrough/stream-bridge/setup-restore.json')
    if log:
        recovery={'pc':str(local.root/'setup-restore.json')}
        if context.get('frame_recovery'):recovery['frame']=context['frame_recovery']
        log.event(context['stage'],'recovery',reason='recovery_retained',recovery=recovery)
    context['paths_printed']=True


def record_failure(log,engine,stage,error):
    if not log:return
    try:
        checks=fixed_checks(Path(engine.__file__).read_bytes(),Path(__file__).read_bytes(),
            *(Path(__file__).with_name(name+'.py').read_bytes() for name in
              ('steam_link_setup_pc','steam_link_setup_frame','steam_link_setup_checkpoint','steam_link_setup_workflow')))
        reason=failure_record(engine,error,checks)['reason']
        log.record_failure(stage,error,reason=reason if reason in checks else None)
    except (OSError,ValueError,TypeError):log.record_failure(stage,error)


def run(args,engine,local,*,input_fn=input,discover_frame=None,remote_factory=None):
    """Own one durable log and one SSH lifetime, including failed installs."""
    log=None;context={'peers':[],'stage':'bundle'}
    if not args.check and not args.dry_run:
        log=load_helper(engine,'steam_link_setup_log').SessionLog.open(local.root,system=local.system)
    context['log']=log
    try:
        if log:log.event('session','start',status='started')
        while True:
            try:
                result=_run(args,engine,local,input_fn=input_fn,discover_frame=discover_frame,
                            remote_factory=remote_factory,context=context)
                if result!=-1:
                    if log:log.event('complete','complete',status='ok' if result==0 else 'failed')
                    return result
            except BaseException as error:
                record_failure(log,engine,context['stage'],error)
                support_paths(local,context)
                if args.check or args.dry_run or not sys.stdin.isatty():raise
                if isinstance(error,KeyboardInterrupt):return 130
                ui=context.get('ui') or engine.load_presentation().Presenter(input_fn=input_fn,lang=args.lang,input_enabled=True)
                if isinstance(error,engine.SetupError):print(str(error),file=sys.stderr)
                ui.say('bridge_retry_question')
                if ui.choose_action(['retry','exit'],recommended='retry')!='retry':return 130 if isinstance(error,KeyboardInterrupt) else 2
            # A preflight/discovery retry is still the same setup session.
            # Keep its admitted live connection instead of asking again.
            context['paths_printed']=False
    finally:
        for peer in context['peers']:
            close=getattr(peer,'close',None)
            if close:
                try:close()
                except (OSError,ValueError):
                    if log:log.event('cleanup','failure',status='failed')
        if context.get('ssh_auth') is not None:context['ssh_auth'].close()
        if log:
            if not context.get('paths_printed'):
                ui=context.get('ui')
                key='setup_log' if log.available else 'setup_log_unavailable'
                if ui:ui.path(log.path,key=key)
                else:print(('Setup log: ' if log.available else 'Setup log could not be created: ')+str(log.path))
            log.close()


def _run(args,engine,local,*,input_fn=input,discover_frame=None,remote_factory=None,context):
    """Called only when this exact package contains the new bundle manifest."""
    e=engine;log=context['log']
    def phase(stage):
        context['stage']=stage
        if hasattr(context.get('ui'),'stage'):context['ui'].stage(stage)
        if log:log.event(stage,'stage',status='started')
    interactive=not args.check and sys.stdin.isatty()
    ui=e.load_presentation().Presenter(input_fn=input_fn,lang=args.lang,input_enabled=True) if interactive else None
    context['ui']=ui
    if ui:
        ui.banner(inspection=False)
        if log and log.path:
            ui.path(log.path,key='setup_log' if log.available else 'setup_log_unavailable')
    phase('bundle');bundle=Bundle(args.bundle,e)
    phase('pc_inventory');pc=local.inventory();installed=LocalInstall(e,local)
    phase('preflight')
    if ui:ui.status('pc_ready' if installed.check(bundle,pc) else 'pc_partial','info')
    remote=frame=None;address=args.frame
    native_factory=remote_factory or (lambda value,login,**kw:Remote(e,value,login,**kw))
    def factory(*values,**kw):
        peer=native_factory(*values,**kw);context['peers'].append(peer)
        if isinstance(peer,Remote):
            peer.log=log
            if ui and getattr(ui,'inline_ssh',False):
                if context.get('ssh_auth') is None:
                    # load_helper intentionally executes reviewed modules with
                    # a virtual __file__. Relative resources must be supplied
                    # by the physical entry point, never inferred from cwd.
                    setup_dir=e.no_links(Path(e.__file__)).parent
                    context['ssh_auth']=load_helper(e,'steam_link_setup_auth').Credentials(ui,setup_dir=setup_dir)
                peer.auth=context['ssh_auth']
                peer.cancelled=ui.cancelled
        return peer
    known=context.get('connection')
    if known and not getattr(getattr(known[1],'session',None),'closed',False):
        address,remote=known
        phase('frame_inventory')
        try:
            with terminal_scope(e,ui,remote):frame=remote.call(request(bundle,'bridge-inventory'))
        except (OSError,ValueError):
            context.pop('connection',None)
            close=getattr(remote,'close',None)
            if close:close()
            raise
    phase('frame_discovery')
    if interactive and not address:
        def validate(candidate,*,interactive=False):
            peer=None;selected=False
            try:
                peer=factory(candidate,args.user,interactive=interactive)
                observed=peer.call(request(bundle,'bridge-inventory'))
                if observed.get('architecture') in ('aarch64','arm64') and (observed.get('runtime_compatible') or observed.get('installed') or observed.get('legacy_installed')):
                    selected=True
                    return peer,observed
            except (OSError,ValueError) as error:
                if interactive and ui:
                    ui.say('bridge_connection_failed')
                    if getattr(error,'_sfnp_public_connection_failure',False) and hasattr(ui,'problem'):
                        ui.problem(str(error))
            finally:
                close=getattr(peer,'close',None)
                if close and not selected:close()
            return None
        presentation=e.load_presentation();finder=discover_frame or presentation.find_frame
        found=finder(validate,cached_hosts=e.remembered_frame_hosts(local),presenter=ui)
        if found:address=found.host;remote,frame=found.value
    if address and frame is None:
        phase('frame_inventory')
        remote=factory(address,args.user,interactive=interactive)
        with terminal_scope(e,ui,remote):frame=remote.call(request(bundle,'bridge-inventory'))
    if frame is not None:
        if remote not in context['peers']:context['peers'].append(remote)
        context['connection']=(address,remote)
        path=frame.get('recovery_path')
        if type(path)is str and path.startswith('/') and path.endswith('/SteamLinkNativePassthrough/stream-bridge/setup-restore.json') and len(path)<=2048 and not any(c in path for c in ('\n','\r','\x00')) and '..' not in path.split('/'):
            context['frame_recovery']=path
    available=bundle.value['readiness']=='release' and bundle.pc['reviewed'] and bundle.frame['reviewed'] and local.system=='Windows'
    state=installed.state()
    pc_versions=version_status(state,bundle.pc)
    frame_versions={'installed':frame.get('installed'),'installed_version':patch_version(frame.get('installed_version')),
                    'target_version':patch_version(bundle.frame.get('version'))} if frame is not None else {
                    'installed':None,'installed_version':None,'target_version':patch_version(bundle.frame.get('version'))}
    versions={'pc':pc_versions,'frame':frame_versions}
    downgrade=any(blocked_version_update(v['target_version'],v['installed_version']) for v in versions.values())
    if log:log.event('preflight','result',status='ok',versions=versions)
    report={'schema':SCHEMA,'bridge_available':available,'automatic_restart':False,'version_update_blocked':downgrade,
            'version':bundle.value.get('version'),'target_versions':{k:v['target_version'] for k,v in versions.items()},
            'pc':{'compatible':installed.check(bundle,pc),'runtime_versions':pc.get('runtime_versions'),**pc_versions},'frame':frame}
    if args.check or args.dry_run:
        print(json.dumps(report,sort_keys=True));return 0
    def display_inventory(current_pc,current_frame):
        if not hasattr(ui,'inventory'):return
        observed_pc=version_status(installed.state(),bundle.pc)
        ui.inventory(pc={'steamvr':str(current_pc.get('steamvr') or ''),'platform':current_pc.get('platform'),
                         'runtime_versions':current_pc.get('runtime_versions'),
                         'compatible':installed.check(bundle,current_pc),**observed_pc},
                     frame={**frame_versions} if current_frame is None else {key:current_frame.get(key) for key in
                         ('installed','installed_version','legacy_installed','runtime_compatible','runtime_versions','enabled','recovery_pending')}|{
                         'host':str(address),'target_version':bundle.frame.get('version')},
                     report={'bridge_available':available,'automatic_restart':False})
    def refresh_inventory():
        # Presentation after a committed operation cannot turn a successful
        # installation into rollback. A lost read-only refresh leaves an honest
        # status notice; the exact completed operation and logs stay intact.
        if not hasattr(ui,'inventory'):return
        try:
            current_pc=local.inventory()
            with terminal_scope(e,ui,remote):current_frame=remote.call(request(bundle,'bridge-inventory'))
            display_inventory(current_pc,current_frame)
        except (OSError,ValueError):ui.say('bridge_inventory_refresh_failed')
    if ui:
        if frame is not None:ui.status('receiver_ready' if frame.get('runtime_compatible') else 'receiver_changed','info')
        ui.status('bridge_available' if available else 'bridge_unavailable','info')
        if downgrade:ui.say('version_downgrade')
        display_inventory(pc,frame)
    actions=['retry','check','exit']
    pending=(local.root/'setup-restore.json').exists() or (local.root/'lifecycle.json').exists() or frame is not None and frame.get('recovery_pending')
    if pending and frame is not None:actions.insert(0,'rollback')
    if frame is not None and (pending or state or frame.get('installed') or frame.get('legacy_installed')):actions.insert(0,'uninstall')
    if available and not downgrade and frame is not None and report['pc']['compatible'] and frame.get('runtime_compatible'):
        if state is None:actions.insert(0,'install')
        elif state['bundle']==bundle.pin and frame.get('files_current') is True:actions.insert(0,'repair')
        else:actions.insert(0,'update')
    recommended='install' if 'install' in actions else 'update' if 'update' in actions else 'repair' if 'repair' in actions else 'rollback' if pending else 'check'
    resumed=context.pop('confirmed_apply',None)
    if resumed is not None:
        e.require(type(resumed)is dict and set(resumed)=={'bundle','intent'} and resumed['bundle']==bundle.pin and
                  resumed['intent'] in ('install','update','repair'),
                  'The recovered operation does not match this checked package.')
        # Recovery can restore the original legacy-only baseline. An update
        # then becomes a fresh installation of the SAME already-confirmed
        # package, rather than silently dropping the user's requested update.
        action=next((candidate for candidate in (resumed['intent'],'install','update','repair') if candidate in actions),None)
        e.require(action is not None,'The confirmed changes are no longer available after recovery; check the reported PC and Frame state.')
        if ui:ui.say('bridge_resuming_update')
    else:
        action=args.action or (ui.choose_action(actions,recommended=recommended) if ui else None)
    if log and action in actions:log.event('preflight','result',action=action,status='pending')
    if action in (None,'exit','check'):
        if action=='check' and ui:ui.say('check_finish')
        return 0
    e.require(interactive and action in actions,'That action is not available for this checked package.')
    if action=='retry':return -1
    def confirm(key,*,remove=False):
        if resumed is not None:return True
        accepted=ui.confirm_uninstall() if remove else ui.confirm(key)
        if log:log.event('preflight','result',action=action,status='ok' if accepted else 'cancelled')
        return accepted
    if action=='rollback':
        ui.say('bridge_recovery_pending')
        if not confirm('bridge_recover_confirm'):return 0
    elif action=='uninstall':
        ui.say('bridge_remove_plan')
        if not confirm('confirm_uninstall',remove=True):return 0
    else:
        bundle.admit(local.system)
        ui.say('bridge_install_plan')
        if hasattr(ui,'version_plan'):ui.version_plan(pc=pc_versions,frame=frame_versions)
        if not confirm('bridge_confirm'):return 0
    context['operation_action']=action
    ports=Automation(e,bundle,local,installed,remote,ui,context,factory,address,args.user)
    flow=load_helper(e,'steam_link_setup_workflow').Workflow(ports,ui)
    if pending and action in ('install','update','repair','uninstall'):
        result=flow.run(action,reconcile=True)
        if result==0:refresh_inventory()
        return result
    if pending or action=='rollback':
        ui.say('bridge_recovery_pending')
        if not flow.recover():return 2
        if action in ('install','update','repair'):
            context['confirmed_apply']={'bundle':bundle.pin,'intent':action}
        if action=='rollback':refresh_inventory()
        return 0 if action=='rollback' else -1
    result=flow.run(action)
    if result==0:refresh_inventory()
    return result
