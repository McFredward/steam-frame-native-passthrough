#!/usr/bin/env python3
"""Assemble the two-sided setup from exact owned artifacts; never install it.

Prototype is the default. Release admission is an explicit release-engineering
step requiring supplied, source/artifact-bound software validation receipts.
Existing online bootstraps intentionally reject this new outer schema until
separately updated; this tool does not disguise it as the legacy atlas package.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]
PROFILE='sfnp-stream-bridge-runtime-profile-v1'
VALIDATION='sfnp-stream-bridge-release-validation-v1'
BOOTSTRAP='sfnp-bootstrap-stream-bridge-v1'
CHECKS=frozenset(('pc_provider_build','frame_layer_build','frame_selector_build',
                 'ordinary_rgba_source','exact_frame_association','opaque_fallback',
                 'owned_coverage_gpu','installer_transaction','startup_health_logic'))
MAX_ARCHIVE=128*1024*1024
MAX_EXPANDED=192*1024*1024


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);sys.modules[name]=value;spec.loader.exec_module(value)
    return value


def pin(raw):return {'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}


def required_sources(root=ROOT):
    bridge=load('sfnp_bridge_package_contract',root/'setup/stream_bridge_install.py')
    return sorted({'setup/steam_link_setup.py','setup/steam_link_setup_ui.py','setup/stream_bridge_install.py',
                   'setup/steam_link_setup_textual.py','setup/steam_link_setup_vendor.py',
                   'setup/steam_link_setup_auth.py','setup/steam_link_setup_askpass_client.py',
                   'setup/steam-link-askpass.sh','setup/steam_link_askpass.c','tools/build_setup_askpass.py',
                   'setup/steam_link_setup_session.py','setup/steam_link_setup_log.py',
                   'setup/steam_link_setup_pc.py','setup/steam_link_setup_frame.py',
                   'setup/steam_link_setup_checkpoint.py','setup/steam_link_setup_workflow.py',
                   'setup/patch-steam-link.cmd','setup/patch-steam-link.sh','LICENSE',
                   'pc/stream-bridge-sender-provider.vrdrivermanifest',
                   'tools/package_stream_bridge.py','tools/package_stream_link_setup.py',
                   'tools/package_setup_dependencies.py','tools/package_setup_textual.py',
                   'packaging/setup-textual.json','packaging/setup-python.json','packaging/setup-openssh.json'}|
                  {name.removeprefix('frame/') for name in bridge.TOOLS|bridge.PROFILES})


def own_artifacts(engine,pc_provider,frame_layer,frame_selector):
    artifacts={name:engine.read(path) for name,path in
               [('pc_provider',pc_provider),('frame_layer',frame_layer),('frame_selector',frame_selector)]}
    pc=artifacts['pc_provider'];engine.require(len(pc)>=128 and pc[:2]==b'MZ','The PC provider must be the reviewed x64 DLL.')
    offset=struct.unpack_from('<I',pc,60)[0]
    engine.require(offset<=len(pc)-26 and pc[offset:offset+4]==b'PE\0\0' and
                   struct.unpack_from('<H',pc,offset+4)[0]==0x8664 and
                   struct.unpack_from('<H',pc,offset+24)[0]==0x20b and
                   struct.unpack_from('<H',pc,offset+22)[0]&0x2000,'Unsupported PC provider architecture or image kind.')
    for name in ('frame_layer','frame_selector'):
        raw=artifacts[name]
        engine.require(len(raw)>=64 and raw[:7]==b'\x7fELF\x02\x01\x01' and
                       struct.unpack_from('<HH',raw,16)==(3,183),'Unsupported Frame shared library architecture or image kind.')
    return artifacts


def validation(engine,profile,profile_raw,artifacts,receipts):
    engine.require(receipts and len(receipts)<=8,'Release packaging needs explicit software validation receipts.')
    matched=set();pins={name:pin(raw) for name,raw in artifacts.items()};accepted=[]
    for path in receipts:
        raw=engine.read(path,128*1024);value=engine.parse(raw)
        engine.require(set(value)=={'schema','result','runtime_profile_sha256','source_revision','source_pins',
                                    'artifacts','checks','hardware_verified'} and
                       value['schema']==VALIDATION and value['result']=='PASS' and
                       value['runtime_profile_sha256']==pin(profile_raw)['sha256'] and
                       value['source_revision']==profile['source_revision'] and
                       value['source_pins']==profile['source_pins'] and value['artifacts']==pins and
                       type(value['checks'])is dict and set(value['checks'])<=CHECKS and value['checks'] and
                       all(v is True for v in value['checks'].values()) and
                       type(value['hardware_verified'])is bool,
                       'A release validation receipt is incomplete or belongs to different artifacts/sources.')
        matched.update(value['checks']);accepted.append({'pin':pin(raw),'checks':sorted(value['checks']),
                                                       'hardware_verified':value['hardware_verified']})
    engine.require(matched==CHECKS,'Required software validation evidence is missing; prototype packaging remains available.')
    return accepted


def build(pc_provider,frame_layer,frame_selector,runtime_profile,python_zip,openssh_zip,
          *,release=False,validation_receipts=(),textual_wheels=None,askpass_exe=None,root=ROOT):
    engine=load('sfnp_bridge_package_engine',root/'setup/steam_link_setup.py')
    bridge=load('sfnp_bridge_package_install',root/'setup/stream_bridge_install.py')
    shared=load('sfnp_bridge_package_runtime',root/'tools/package_stream_link_setup.py')
    deps=load('sfnp_bridge_package_dependencies',root/'tools/package_setup_dependencies.py')
    raw_profile=engine.read(runtime_profile,128*1024);profile=engine.parse(raw_profile)
    engine.require(set(profile)=={'schema','bridge_protocol','source_revision','source_pins','artifacts','pc','frame'} and
                   profile['schema']==PROFILE and type(profile['source_revision'])is str and
                   re.fullmatch('[0-9a-f]{40}',profile['source_revision']), 'Unknown runtime profile or source revision.')
    engine.protocol(profile['bridge_protocol'])
    sources=required_sources(root);engine.records(profile['source_pins'])
    engine.require(set(profile['source_pins'])==set(sources),'The runtime profile lacks exact transitive owned setup sources.')
    source_bytes={name:engine.read(root/engine.relative(name)) for name in sources}
    engine.require({name:pin(raw) for name,raw in source_bytes.items()}==profile['source_pins'],
                   'Owned setup or generator sources differ from the reviewed profile.')
    artifacts=own_artifacts(engine,pc_provider,frame_layer,frame_selector)
    engine.require(profile['artifacts']=={name:pin(raw) for name,raw in artifacts.items()},
                   'A native artifact differs from the reviewed runtime profile.')
    pc,frame=profile['pc'],profile['frame']
    engine.require(type(pc)is dict and set(pc)=={'reviewed','platform','runtime_files'} and
                   type(frame)is dict and set(frame)=={'reviewed','runtime_files','generated'} and
                   frame['generated']==bridge.GENERATED,'Unsupported paired runtime profile.')
    receipts=validation(engine,profile,raw_profile,artifacts,validation_receipts) if release else []
    engine.require(not release or pc['reviewed'] is True and frame['reviewed'] is True,
                   'Both runtime roles must be reviewed before release packaging.')
    payload={'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll':artifacts['pc_provider'],
             'pc-provider/driver.vrdrivermanifest':source_bytes['pc/stream-bridge-sender-provider.vrdrivermanifest'],
             'frame/'+bridge.LIBRARY:artifacts['frame_layer'],
             'frame/libsfnp_stream_launch.so':artifacts['frame_selector']}
    payload.update({name:source_bytes[name.removeprefix('frame/')] for name in bridge.TOOLS|bridge.PROFILES})
    bundle={'schema':bridge.SCHEMA,'readiness':'release' if release else 'prototype',
            'bridge_protocol':profile['bridge_protocol'],'files':{name:pin(raw) for name,raw in sorted(payload.items())},
            'pc':{**pc,'files':sorted(bridge.PC_FILES)},'frame':{**frame,'files':sorted(name for name in payload if name.startswith('frame/'))}}
    # Exercise the actual installed-engine manifest parser. This publishes no
    # native shader, changes no provider registration and never calls dlopen.
    with tempfile.TemporaryDirectory(prefix='sfnp-package-contract-') as temporary:
        candidate=Path(temporary)
        for name,raw in payload.items():engine.put(candidate/name,raw)
        engine.put(candidate/'stream-bridge-bundle.json',engine.encoded(bundle))
        parsed=bridge.Bundle(candidate,engine)
        if release:parsed.admit('Windows')
    files={**payload,'stream-bridge-bundle.json':engine.encoded(bundle),
           'setup/steam_link_setup.py':source_bytes['setup/steam_link_setup.py'],
           'setup/steam_link_setup_ui.py':source_bytes['setup/steam_link_setup_ui.py'],
           'setup/steam_link_setup_textual.py':source_bytes['setup/steam_link_setup_textual.py'],
           'setup/steam_link_setup_vendor.py':source_bytes['setup/steam_link_setup_vendor.py'],
           'setup/stream_bridge_install.py':source_bytes['setup/stream_bridge_install.py'],
           'setup/steam_link_setup_session.py':source_bytes['setup/steam_link_setup_session.py'],
           'setup/steam_link_setup_log.py':source_bytes['setup/steam_link_setup_log.py'],
           'setup/steam_link_setup_pc.py':source_bytes['setup/steam_link_setup_pc.py'],
           'setup/steam_link_setup_frame.py':source_bytes['setup/steam_link_setup_frame.py'],
           'setup/steam_link_setup_checkpoint.py':source_bytes['setup/steam_link_setup_checkpoint.py'],
           'setup/steam_link_setup_workflow.py':source_bytes['setup/steam_link_setup_workflow.py'],
           'patch-steam-link.cmd':source_bytes['setup/patch-steam-link.cmd'],
           'patch-steam-link.sh':source_bytes['setup/patch-steam-link.sh'],
           'LICENSE':source_bytes['LICENSE']}
    for name in ('steam_link_setup_auth.py','steam_link_setup_askpass_client.py','steam-link-askpass.sh'):
        files['setup/'+name]=source_bytes['setup/'+name]
    engine.require(askpass_exe is not None,'The included Windows sign-in helper is required.')
    helper=engine.read(askpass_exe,1024*1024)
    load('sfnp_askpass_package_check',root/'tools/build_setup_askpass.py').inspect(helper)
    files['setup/steam-link-askpass.exe']=helper
    py_pin=engine.parse(source_bytes['packaging/setup-python.json']);py_raw=engine.read(python_zip)
    engine.require(len(py_raw)==py_pin['bytes'],'Portable Python archive size differs from its official pin.')
    files.update(shared.windows_python(py_raw,py_pin['sha256'],py_pin['source_url']))
    ssh_pin=engine.parse(source_bytes['packaging/setup-openssh.json'])
    files.update(deps.windows_openssh(engine.read(openssh_zip),ssh_pin))
    textual=load('sfnp_bridge_textual_packaging',root/'tools/package_setup_textual.py')
    files.update(textual.vendor(textual_wheels,engine.parse(source_bytes['packaging/setup-textual.json'])))
    source_manifest={'schema':'sfnp-stream-bridge-package-source-v1','source_revision':profile['source_revision'],
                     'runtime_profile':pin(raw_profile),'source_pins':profile['source_pins'],
                     'native_artifacts':profile['artifacts'],'validation_receipts':receipts,
                     'hardware_verified':False,'automatic_restart':bool(release)}
    files['stream-bridge-package-source.json']=engine.encoded(source_manifest)
    files['README.txt']=('Steam Link Native Passthrough - '+('reviewed setup' if release else 'prototype setup')+'\n\n'+
        ('Run patch-steam-link.cmd on Windows. Confirmed setup prepares Frame files while its interface stays running, then coordinates and monitors the required restart.\n'
         if release else 'Checks only: installation and pairing are deliberately blocked in this prototype.\n')+
        'The Linux PC sender is not included. Native-derived shaders are generated locally, never shipped.\n'+
        'Setup closes only verified PC SteamVR processes and leaves PC SteamVR closed. It restarts/checks the Frame interface and asks you to confirm the picture.\n'+
        'A failed start restores the exact original operation checkpoint and restarts the original Frame interface. Incomplete recovery offers Retry.\n'+
        'Use the current guided paired setup. There is no Frame-only atlas fallback.\n').encode()
    engine.require(len(files)<4096 and sum(map(len,files.values()))<=MAX_EXPANDED,'Setup package exceeds its bounded inventory.')
    outer={'schema':BOOTSTRAP,'readiness':bundle['readiness'],'windows_python':'setup/runtime/python/python.exe',
           'files':{name:pin(raw) for name,raw in sorted(files.items())}}
    files['bootstrap-package.json']=engine.encoded(outer)
    output=io.BytesIO()
    with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for name,raw in sorted(files.items()):
            engine.relative(name);engine.require(0<=len(raw)<=engine.MAX_FILE,'Oversized setup member.')
            engine.require(not name.endswith('.spv'),'Native-derived shader payloads cannot be distributed.')
            info=zipfile.ZipInfo(name,date_time=(2026,10,18,0,0,0));info.external_attr=(0o100700 if name=='setup/steam-link-askpass.sh' else 0o100600)<<16
            archive.writestr(info,raw,compress_type=zipfile.ZIP_DEFLATED,compresslevel=6)
    raw=output.getvalue();engine.require(len(raw)<=MAX_ARCHIVE,'Setup archive exceeds download limit.')
    receipt={'schema':'sfnp-stream-bridge-packaging-result-v1','result':'PASS','archive':pin(raw),
             'readiness':bundle['readiness'],'native_installable':bool(release),
             'source_revision':profile['source_revision'],'runtime_profile':pin(raw_profile),
             'source_pins':profile['source_pins'],'artifacts':profile['artifacts'],
             'validation_receipts':receipts,'hardware_verified':False,
             'online_bootstrap_currently_accepts_schema':bool(release),'device_actions':False,
             'temporary_contract_tree_removed':True}
    return raw,receipt


def publish(path,raw):
    path.parent.mkdir(parents=True,exist_ok=True)
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    try:
        with os.fdopen(fd,'wb',closefd=False) as stream:stream.write(raw);stream.flush();os.fsync(fd)
    finally:os.close(fd)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('pc-provider','frame-layer','frame-selector','runtime-profile','python-zip','openssh-zip','textual-wheels','askpass-exe','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--release',action='store_true')
    parser.add_argument('--validation-receipt',action='append',type=Path,default=[])
    args=parser.parse_args(argv)
    raw,receipt=build(args.pc_provider,args.frame_layer,args.frame_selector,args.runtime_profile,args.python_zip,args.openssh_zip,
                      release=args.release,validation_receipts=args.validation_receipt,textual_wheels=args.textual_wheels,askpass_exe=args.askpass_exe)
    sidecar=args.output.with_suffix(args.output.suffix+'.receipt.json')
    if args.output.exists() or sidecar.exists():raise ValueError('An output or receipt already exists; it was preserved.')
    publish(args.output,raw)
    try:publish(sidecar,(json.dumps(receipt,sort_keys=True,indent=2)+'\n').encode())
    except BaseException:
        # Keep the completed exact archive. A missing receipt is not silently
        # relabeled as a deployable release; rerunning never replaces it.
        raise
    print(json.dumps({'artifact':str(args.output),'receipt':str(sidecar),**receipt},sort_keys=True))
    return 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,KeyError,TypeError,zipfile.BadZipFile) as error:
        print('Bridge packaging refused: '+str(error)[:500],file=sys.stderr);raise SystemExit(2)
