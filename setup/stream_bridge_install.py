"""Exact-package full-resolution setup with confirmed lifecycle orchestration.

This module is loaded by the verified setup entry. Prototype manifests provide
inventory only. Source identities and successful software tests do not turn a
prototype into an installable release.
"""
from __future__ import annotations
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
GENERATED = {'coverage.spv': {'bytes':7188,'sha256':'316ceb1cc3c50a6eef9c6e176d1001d1afd61e0747b628d87728439c3d2bfc6e'},
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
                checkpoint.begin('repair',[checkpoint.operation(path,[helper.file_value(raw)]) for path,raw in missing],[],bool(pc.get('registered')))
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
            receipt={'schema':RECEIPT,'bundle':bundle.pin,'files':{n:bundle.value['files'][n] for n in PC_FILES},
                     'settings':{'path':str(self.settings_path()),'baseline':baseline},'registered_before':bool(pc.get('registered')),
                     'pair':pair['pair_id'],'vrpathreg':tool_pin}
            planned_ops.append(checkpoint.operation(self.receipt,[helper.file_value(e.encoded(receipt))]))
            settings=[checkpoint.setting(self.settings_path(),SETTINGS_SECTION,k,[{'present':True,'value':v}]) for k,v in edits.edits[SETTINGS_SECTION].items()]
            checkpoint.begin('install',planned_ops,settings,bool(pc.get('registered')))
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

    def update(self,bundle,pc,pair,address,port,old,tool):
        """Retain the initial uninstall baseline and checkpoint this update."""
        e=self.e;helper=load_helper(e,'steam_link_setup_checkpoint')
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
        receipt_before=e.read(self.receipt,2*1024*1024,private=True);receipt_after=e.encoded(replacement)
        checkpoint=self.checkpoint(pair['pair_id'])
        operations=[checkpoint.operation(path,[helper.file_value(raw)]) for path,_,raw in changes]
        operations.append(checkpoint.operation(self.receipt,[helper.file_value(receipt_after)]))
        settings=[checkpoint.setting(settings_path,section,key,[{'present':True,'value':value}])
                  for section,values in desired.items() for key,value in values.items()]
        checkpoint.begin('update',operations,settings,bool(pc.get('registered')))
        for path,previous,raw in changes:e.put(path,raw,previous)
        mode=stat.S_IMODE(settings_path.stat().st_mode)
        e.put(settings_path,e.encoded(preferences),before)
        if os.name!='nt':os.chmod(settings_path,mode)
        e.put(self.receipt,receipt_after,receipt_before)
        if not pc.get('registered'):
            self.local.register(tool,'adddriver')
            e.require(self.local.inventory()['registered'],'The updated provider registration could not be confirmed.')
        return {'reason':'updated','changed':True,'automatic_restart':False}

    def uninstall(self,pc,*,backup=False):
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
        if backup:
            helper=load_helper(e,'steam_link_setup_checkpoint');checkpoint=self.checkpoint(state['pair'])
            paths=[self.local.root/name for name in state['files']]+[self.local.root/'pairings/active.json',self.receipt]
            operations=[checkpoint.operation(path,[None]) for path in paths]
            settings=[checkpoint.setting(self.settings_path(),section,key,[{'present':entry['present'],'value':entry['value']}])
                      for section,spec in state['settings']['baseline'].items() for key,entry in spec['keys'].items()]
            checkpoint.begin('uninstall',operations,settings,bool(pc.get('registered')))
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

    def checkpoint(self):
        helper=load_helper(self.e,'steam_link_setup_checkpoint')
        external={self.home/'.local/share/Steam/config/steamvr.vrsettings',
                  self.home/'.config/systemd/user/steamvr.service.d/80-steam-frame-native-passthrough.conf',
                  self.home/'.local/share/vulkan/implicit_layer.d/VkLayer_SteamFrameNativePassthrough.json',
                  self.home/'.config/SteamFrameNativePassthrough/control.json',
                  self.config/'vulkan/implicit_layer.d'/MANIFEST,
                  self.config/'systemd/user/steam.service.d/85-steam-link-native-passthrough.conf'}
        names={LIBRARY,'libsfnp_stream_launch.so','coverage.spv','legacy.spv','receiver.conf','launch.conf',
               'receiver.enable','pairings/active.json','install.json'}|{n.removeprefix('frame/') for n in TOOLS|PROFILES}
        def allowed(path):
            if path in external:return True
            if not path.is_relative_to(self.root):return False
            name=path.relative_to(self.root).as_posix()
            return name in names or re.fullmatch('pairings/[0-9a-f]{32}[.]json',name) is not None
        def current(path):self.tx.owned(path);return helper.read_value(self.e,path)
        def replace(path,old,new):self.tx.owned(path);return helper.replace_value(self.e,path,old,new)
        return helper.Checkpoint(self.e,self.root/'setup-restore.json',allowed,current,replace)

    def backup(self,action,operations,settings,transaction_values):
        helper=load_helper(self.e,'steam_link_setup_checkpoint');checkpoint=self.checkpoint()
        entries=[]
        for op in operations:
            after=op['after']
            if after and after['kind']=='file':after={**after,'mode':op['mode']}
            entries.append(checkpoint.operation(Path(op['path']),[after]))
        entries.append(checkpoint.operation(self.tx.path,transaction_values))
        entries_settings=[checkpoint.setting(Path(settings['path']),section,key,[{'present':entry['present'],'value':entry['value']}]
                       if action=='uninstall' else [{'present':True,'value':entry['installed']}])
                       for section,spec in settings['baseline'].items() for key,entry in spec['keys'].items()]
        checkpoint.begin(action,entries,entries_settings)

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
        settings=self.home/'.local/share/Steam/config/steamvr.vrsettings'
        change=SettingsChange(e,settings,{'camera':{'forceSceneAlphaBlending':True}})
        before,after,baseline=change.planned()
        # Settings get a separate key-level receipt; do not restore the whole
        # SteamVR file after the player changes unrelated preferences later.
        return operations,{'path':str(settings),'baseline':baseline},(before,after)

    def install(self,bundle,pair,address,port,*,backup=False,live=False):
        e=self.e;current=self.tx.state()
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
                self.backup('repair',missing,current['settings'],[helper.read_value(e,self.tx.path)])
            for operation in missing:self.tx.replace(operation)
            self.system('daemon-reload')
            return {'reason':'repaired','automatic_restart':False,'manual_restart_required':True}
        operations,settings,(before,after)=self.plan(bundle,pair,address,port,live=live)
        if backup:
            helper=load_helper(e,'steam_link_setup_checkpoint')
            transaction={'schema':RECEIPT,'home':str(self.home),'root':str(self.root),'bundle':bundle.pin,
                         'phase':'pending','operations':operations,'settings':settings,'pair':pair['pair_id']}
            pending=helper.file_value(e.encoded(transaction));transaction['phase']='installed'
            self.backup('install',operations,settings,[pending,helper.file_value(e.encoded(transaction)),None])
        self.tx.begin(bundle.pin,operations,settings,pair['pair_id'])
        try:
            settings_path=Path(settings['path']);mode=stat.S_IMODE(settings_path.stat().st_mode) if settings_path.exists() else 0o600
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
        settings=current['settings'];preferences=e.parse(e.read(Path(settings['path']),2*1024*1024))
        for section,spec in settings['baseline'].items():
            values=preferences.get(section)
            e.require(type(values)is dict and all(key in values and type(values[key])is type(entry['installed']) and
                      values[key]==entry['installed'] for key,entry in spec['keys'].items()),
                      'Changed Frame settings preserved; update stopped.')
        with tempfile.TemporaryDirectory(prefix='prepare-update-',dir=self.root.parent) as stage:
            outputs=generate_shaders(e,bundle,original,Path(stage))
        wanted={self.root/LIBRARY:bundle.files['frame/'+LIBRARY],
                self.root/'libsfnp_stream_launch.so':bundle.files['frame/libsfnp_stream_launch.so'],
                self.root/'receiver.conf':MARKER+address.encode()+b'\n'+str(port).encode()+b'\n'}
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
                observed=self.tx.current(path)
                if observed!=op['after']:changes.append({**op,'before':observed})
        pending={**current,'bundle':bundle.pin,'operations':operations,'phase':'pending'}
        committed={**pending,'phase':'installed'};helper=load_helper(e,'steam_link_setup_checkpoint')
        previous=e.read(self.tx.path,64*1024*1024,private=True)
        self.backup('update',changes,settings,[helper.file_value(e.encoded(pending)),helper.file_value(e.encoded(committed))])
        e.put(self.tx.path,e.encoded(pending),previous)
        for op in changes:self.tx.replace(op)
        self.system('daemon-reload')
        return {'reason':'prepared-update','pair_id':current['pair'],'automatic_restart':False,
                'manual_restart_required':True,'coverage_operational':False}

    def undo(self,*,backup=False,live=False):
        state=self.tx.state()
        if state is None:return {'reason':'not_installed','automatic_restart':False}
        if not live:self.cold(steam=True)
        if backup:
            operations=[{**op,'after':op['before']} for op in state['operations']]
            self.backup('uninstall',operations,state['settings'],[None])
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
    if command in ('bridge-runtime-snapshot','bridge-runtime-restart','bridge-runtime-restored'):
        lifecycle=load_helper(e,'steam_link_setup_frame').FrameLifecycle(e,unit.root)
        if command=='bridge-runtime-snapshot':return lifecycle.snapshot()
        if command=='bridge-runtime-restart':return lifecycle.restart()
        return lifecycle.restored(health_ok=request.get('health_ok') is True)
    if command=='bridge-restore':
        result=unit.checkpoint().restore();unit.system('daemon-reload');return result
    if command=='bridge-checkpoint-finish':unit.checkpoint().finish(restored=request.get('restored') is True);return {'reason':'complete'}
    if command=='bridge-undo':return unit.undo(backup=request.get('automated') is True,live=request.get('automated') is True)
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
        self.engine_source=Path(engine.__file__).read_bytes()
        self.bridge_source=Path(__file__).read_bytes()
        self.helper_sources={name:Path(__file__).with_name(name+'.py').read_bytes() for name in
                             ('steam_link_setup_checkpoint','steam_link_setup_frame')}
        self.checks=fixed_checks(self.engine_source,self.bridge_source,*self.helper_sources.values())

    def close(self):
        if self.session is not None:self.session.close()

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
                    f'checks=b.fixed_checks(base64.b64decode({base64.b64encode(self.engine_source).decode()!r}),bridge_source,*helper_sources)\n'
                    'def dispatch(request):\n'
                    ' try:return b.remote_request(e,request)\n'
                    ' except Exception as error:return {"error":True,"failure":b.failure_record(e,error,checks)}\n')
            session=load_helper(e,'steam_link_setup_session')
            self.session=session.Session(e.ssh_args(self.address,self.login,interactive=self.interactive),source,interactive=self.interactive,timeout=90)
        try:result=self.session.call(request)
        except (OSError,ValueError,TimeoutError) as error:
            if self.log:self.log.record_failure(stage,error,command=command)
            raise e.SetupError('The SSH session ended or timed out. The operation was not retried; run setup again to check its recovery records.') from None
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
            raise e.SetupError(message)
        if self.log:self.log.event(stage,'result',command=command,status='ok')
        return result


def request(bundle,command,**values):
    result={'command':command,'manifest':bundle.value,**values}
    if command=='bridge-prepare':
        result['files']={name:{**bundle.value['files'][name],
            'data':base64.b64encode(raw).decode()} for name,raw in bundle.files.items()}
    if command in ('bridge-health-baseline','bridge-health'):
        result['health_source']=base64.b64encode(bundle.files['frame/tools/compositor_startup_health.py']).decode()
    return result


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
        log=self.context['log']
        if log:log.event(stage,'stage',status='started')

    def call(self,command,**values):
        # Reconnection is confined to a deliberate operation/recovery attempt.
        # It never blindly resends an uncertain installation request.
        if getattr(getattr(self.remote,'session',None),'closed',False):
            self.remote=self.factory(self.address,self.login,interactive=True)
        with self.ui.suspend():return self.remote.call(request(self.bundle,command,**values))

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
            self.installed.uninstall(self.local.inventory(),backup=True)
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
        self.phase('runtime_start');self.call('bridge-runtime-restart')

    def prepared_health(self):
        self.phase('startup_health');result=self.call('bridge-health',baseline=self.baseline)
        return result

    def restore_files(self):
        self.phase('rollback')
        # A PC-side recovery error must never strand the Frame in a broken
        # startup. Restore/restart/check its UI independently, then report any
        # remaining PC recovery failure without claiming complete rollback.
        self.call('bridge-restore')
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

    def restart_original(self):
        self.phase('runtime_start');self.restore_baseline=self.call('bridge-health-baseline')
        self.call('bridge-runtime-restart')
        if self.pc_recovery_error is None:
            try:self.runtime.restore()
            except Exception as error:
                self.pc_recovery_error=error
                record_failure(self.context['log'],self.e,'rollback',error)

    def original_health(self):
        self.phase('startup_health');result=self.call('bridge-health',baseline=self.restore_baseline)
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
        if log:log.event(stage,'stage',status='started')
    phase('bundle');bundle=Bundle(args.bundle,e)
    phase('pc_inventory');pc=local.inventory();installed=LocalInstall(e,local)
    phase('preflight')
    interactive=not args.check and sys.stdin.isatty()
    ui=e.load_presentation().Presenter(input_fn=input_fn,lang=args.lang,input_enabled=True) if interactive else None
    context['ui']=ui
    if ui:ui.banner(inspection=False);ui.status('pc_ready' if installed.check(bundle,pc) else 'pc_partial','info')
    remote=frame=None;address=args.frame
    native_factory=remote_factory or (lambda value,login,**kw:Remote(e,value,login,**kw))
    def factory(*values,**kw):
        peer=native_factory(*values,**kw);context['peers'].append(peer)
        if isinstance(peer,Remote):peer.log=log
        return peer
    known=context.get('connection')
    if known and not getattr(getattr(known[1],'session',None),'closed',False):
        address,remote=known
        phase('frame_inventory')
        try:
            with ui.suspend() if ui else e.nullcontext():frame=remote.call(request(bundle,'bridge-inventory'))
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
            except (OSError,ValueError):pass
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
        with ui.suspend() if ui else e.nullcontext():frame=remote.call(request(bundle,'bridge-inventory'))
    if frame is not None:
        if remote not in context['peers']:context['peers'].append(remote)
        context['connection']=(address,remote)
        path=frame.get('recovery_path')
        if type(path)is str and path.startswith('/') and path.endswith('/SteamLinkNativePassthrough/stream-bridge/setup-restore.json') and len(path)<=2048 and not any(c in path for c in ('\n','\r','\x00')) and '..' not in path.split('/'):
            context['frame_recovery']=path
    available=bundle.value['readiness']=='release' and bundle.pc['reviewed'] and bundle.frame['reviewed'] and local.system=='Windows'
    state=installed.state()
    report={'schema':SCHEMA,'bridge_available':available,'automatic_restart':False,
            'pc':{'compatible':installed.check(bundle,pc),'installed':state is not None},'frame':frame}
    if args.check or args.dry_run:
        print(json.dumps(report,sort_keys=True));return 0
    if ui:
        if frame is not None:ui.status('receiver_ready' if frame.get('runtime_compatible') else 'receiver_changed','info')
        ui.status('bridge_available' if available else 'bridge_unavailable','info')
    actions=['retry','check','exit']
    pending=(local.root/'setup-restore.json').exists() or (local.root/'lifecycle.json').exists() or frame is not None and frame.get('recovery_pending')
    if pending and frame is not None:actions.insert(0,'rollback')
    if frame is not None and (state or frame.get('installed')):actions.insert(0,'uninstall')
    if available and frame is not None and report['pc']['compatible'] and frame.get('runtime_compatible'):
        if state is None:actions.insert(0,'install')
        elif state['bundle']==bundle.pin:actions.insert(0,'repair')
        else:actions.insert(0,'update')
    recommended='rollback' if pending else 'install' if 'install' in actions else 'update' if 'update' in actions else 'repair' if 'repair' in actions else 'check'
    action=args.action or (ui.choose_action(actions,recommended=recommended) if ui else None)
    if action in (None,'exit','check'):return 0
    e.require(interactive and action in actions,'That action is not available for this checked package.')
    if action=='retry':return -1
    if action=='rollback':
        ui.say('bridge_recovery_incomplete')
        if not ui.confirm('bridge_recover_confirm'):return 0
    elif action=='uninstall':
        ui.say('bridge_remove_plan')
        if not ui.confirm_uninstall():return 0
    else:
        bundle.admit(local.system)
        ui.say('bridge_install_plan')
        if not ui.confirm('bridge_confirm'):return 0
    ports=Automation(e,bundle,local,installed,remote,ui,context,factory,address,args.user)
    flow=load_helper(e,'steam_link_setup_workflow').Workflow(ports,ui)
    if pending or action=='rollback':
        ui.say('bridge_recovery_incomplete')
        if not flow.recover():return 2
        return 0 if action=='rollback' else -1
    return flow.run(action)
