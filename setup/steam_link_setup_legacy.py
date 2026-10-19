"""Exact removal authority for the retired Frame-only installation.

Uninstall removes every owned generation; Undo remains the separate operation
checkpoint. No old executable is run, no guessed camera baseline is written,
and this module never stops or starts a service.
"""
from __future__ import annotations
import base64
from pathlib import Path
import os
import re
import tempfile

RECEIPT='sfnp-user-install-v1'
CAMERA='sfnp-camera-key-v1'
MANIFEST='VkLayer_SteamFrameNativePassthrough.json'
DROPIN='80-steam-frame-native-passthrough.conf'


def allowed(home,path):
    home,path=Path(home),Path(path)
    config=home/'.config/SteamFrameNativePassthrough'
    data=home/'.local/share/SteamFrameNativePassthrough'
    return path in {config/'install.json',config/'control.json',config/'camera-before.json',
        home/'.config/systemd/user/steamvr.service.d'/DROPIN,
        home/'.local/share/vulkan/implicit_layer.d'/MANIFEST,data/'session.conf'} or any(
        path.is_relative_to(data/name) for name in ('package','tools'))


def plan(unit):
    e=unit.e;home=unit.home;config=home/'.config/SteamFrameNativePassthrough'
    data=home/'.local/share/SteamFrameNativePassthrough';receipt=config/'install.json'
    dropin=home/'.config/systemd/user/steamvr.service.d'/DROPIN
    manifest=home/'.local/share/vulkan/implicit_layer.d'/MANIFEST
    if not os.path.lexists(receipt):
        e.require(not os.path.lexists(dropin) and not os.path.lexists(manifest),
            'Unknown old Frame activation was preserved; it could not be uninstalled safely.')
        return None
    raw=e.read(receipt,128*1024,private=True);value=e.parse(raw)
    expected_link=str(Path('/run/user')/str(os.getuid())/'steam-frame-native-passthrough/receiver.json')
    e.require(type(value)is dict and set(value)=={'schema','home','data','manifest_link','package_sha256','files'} and
        value['schema']==RECEIPT and value['home']==str(home) and value['data']==str(data) and
        value['manifest_link']==expected_link and type(value['package_sha256'])is str and
        re.fullmatch('[0-9a-f]{64}',value['package_sha256']) and type(value['files'])is dict and
        1<=len(value['files'])<=32,'The old Frame installation receipt was not recognized; existing files were preserved.')
    paired=unit.tx.state();admitted={op['path']:op for op in paired['operations']} if paired else {}
    operations=[]
    for name,pin in value['files'].items():
        path=Path(name);unit.tx.owned(path)
        e.require(allowed(home,path) and (path.is_relative_to(data) or path==dropin) and
            type(pin)is str and re.fullmatch('[0-9a-f]{64}',pin),
            'An old Frame component is outside the verified uninstall scope; it was preserved.')
        observed=unit.tx.current(path)
        if observed is None:
            prior=admitted.get(name)
            e.require(prior is not None and prior['after'] is None and prior['before'] is not None and
                prior['before']['kind']=='file' and e.sha(base64.b64decode(prior['before']['bytes'],validate=True))==pin,
                'An old Frame component is missing without an owned migration record; existing files were preserved.')
        else:e.require(observed['kind']=='file' and e.sha(base64.b64decode(observed['bytes'],validate=True))==pin,
            'An old Frame component changed; existing files were preserved.')
        operations.append(unit.tx.operation(path,None,expected=observed))
    if os.path.lexists(manifest):
        observed=unit.tx.current(manifest)
        e.require(observed=={'kind':'link','target':expected_link},
            'An unknown old Frame layer link was preserved.')
        operations.append(unit.tx.operation(manifest,None,expected=observed))
    else:
        # Include an absent migration-owned activation: paired rollback must not
        # reactivate it during full Uninstall.
        operations.append(unit.tx.operation(manifest,None))
    control=config/'control.json'
    if os.path.lexists(control):
        value=e.parse(e.read(control,4096,private=True))
        e.require(type(value)is dict and set(value)=={'schema','enabled'} and
            value['schema']=='sfnp-user-control-v1' and type(value['enabled'])is bool,
            'Unknown old Frame preferences were preserved.')
        operations.append(unit.tx.operation(control,None,expected=unit.tx.current(control)))
    camera=None;camera_receipt=config/'camera-before.json'
    if os.path.lexists(camera_receipt):
        value=e.parse(e.read(camera_receipt,16384,private=True))
        e.require(type(value)is dict and set(value)=={'schema','settings','previous','write_intent'} and
            value['schema']==CAMERA and value['settings']==str(unit.settings_path()) and type(value['write_intent'])is bool,
            'The original camera preference could not be verified; recovery records were preserved.')
        previous=value['previous']
        e.require(type(previous)is dict and set(previous)=={'present','value'} and type(previous['present'])is bool and
            (type(previous['value'])is bool if previous['present'] else previous['value'] is None),
            'The recorded original camera preference is invalid; it was preserved.')
        desired=previous['value'] if previous['present'] else None
        current=unit.camera_value(Path(value['settings']))
        e.require(current is desired or value['write_intent'] is True and current is True,
            'The camera preference changed outside the old patch; it was preserved.')
        prefs=e.parse(e.read(Path(value['settings']),2*1024*1024))
        camera={'schema':'sfnp-camera-override-authority-v1','settings':value['settings'],
            'before':current,'desired':desired,'section_present':'camera' in prefs}
        operations.append(unit.tx.operation(camera_receipt,None,expected=unit.tx.current(camera_receipt)))
    operations.append(unit.tx.operation(receipt,None,expected=unit.tx.current(receipt)))
    e.require(len({op['path']for op in operations})==len(operations),
        'Duplicate old Frame uninstall operations were preserved.')
    return {'operations':operations,'camera':camera}


def apply_camera(unit,removal):
    authority=removal['camera'] if removal else None
    if authority is None:return
    e=unit.e;current=unit.camera_value(Path(authority['settings']))
    e.require(current is authority['before'] or current is authority['desired'],
        'The camera preference changed during uninstall; it was preserved.')
    payload=getattr(unit,'_camera_payload',None)
    e.require(payload is not None,'The verified native camera cleanup helper is required before uninstall.')
    source,library=payload
    module=unit.camera_helper();unit.root.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
    with tempfile.TemporaryDirectory(prefix='retired-camera-restore-',dir=unit.root.parent) as temporary:
        root=Path(temporary);os.chmod(root,0o700)
        e.put(root/'camera-session.json',e.encoded({'schema':'sfnp-camera-session-config-v1',
            'settings':authority['settings'],'library':library}))
        unit.camera_call((module,root),'apply_owned_override',{**authority,'before':current})
