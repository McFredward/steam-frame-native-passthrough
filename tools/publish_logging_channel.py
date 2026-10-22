#!/usr/bin/env python3
"""Retarget only package logging defaults; preserve every native/setup byte.

Publishing is used by branch CI after a maintainer push. Ordinary import and
retarget() have no Git/network/device side effects. No release tags are created.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]
ARCHIVE='distribution/steam-link-native-passthrough.zip'
DEFAULTS=('pc-provider/logging-default.conf','frame/logging-default.conf')

def pin(raw):return {'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
def encoded(value):return (json.dumps(value,sort_keys=True,separators=(',',':'))+'\n').encode()

def retarget(raw,channel):
    if channel not in ('main','dev'):raise ValueError('Unknown channel')
    if len(raw)>128*1024*1024:raise ValueError('Oversized archive')
    files={}
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        if len(z.infolist())>4096:raise ValueError('Archive inventory too large')
        folded=set();expanded=0
        for entry in z.infolist():
            name=entry.filename
            if name.casefold() in folded or name.startswith('/') or '\\' in name or any(p in ('','.','..') for p in name.split('/')):raise ValueError('Unsafe package path')
            folded.add(name.casefold());expanded+=entry.file_size
            if entry.file_size>32*1024*1024 or expanded>192*1024*1024 or entry.external_attr>>16&0o170000 not in (0,0o100000):raise ValueError('Unsafe package member')
            files[name]=z.read(entry)
    outer=json.loads(files['bootstrap-package.json'])
    if outer['schema']!='sfnp-bootstrap-stream-bridge-v1' or outer['files']!={n:pin(b) for n,b in files.items() if n!='bootstrap-package.json'}:raise ValueError('Package integrity differs')
    bundle=json.loads(files['stream-bridge-bundle.json']);source=json.loads(files['stream-bridge-package-source.json'])
    if bundle.get('logging') is None:raise ValueError('Package predates configurable logging; rebuild it first')
    for name in DEFAULTS:
        if bundle['files'].get(name)!=pin(files[name]):raise ValueError('Logging payload differs')
    logging={'channel':channel,'level':'Debug' if channel=='dev' else 'Important'}
    if any(pin(files[name])!=expected for name,expected in bundle['files'].items()):raise ValueError('Native bundle integrity differs')
    native={'pc_provider':'pc-provider/bin/win64/driver_steam_link_native_passthrough.dll','frame_layer':'frame/libVkLayer_steam_link_bridge.so','frame_selector':'frame/libsfnp_stream_launch.so'}
    if source['native_artifacts']!={role:pin(files[name]) for role,name in native.items()}:raise ValueError('Native artifact provenance differs')
    if bundle['logging']==logging:return raw,False
    before=dict(files)
    for name in DEFAULTS:
        files[name]=(logging['level']+'\n').encode();bundle['files'][name]=pin(files[name])
    bundle['logging']=logging;source['logging']=logging
    files['stream-bridge-bundle.json']=encoded(bundle);files['stream-bridge-package-source.json']=encoded(source)
    text=files['README.txt'].decode();text,n=re.subn(r'(?m)^Logging default: .+$','Logging default: '+logging['level']+' ('+channel+').',text)
    if n!=1:raise ValueError('Package logging notice missing')
    files['README.txt']=text.encode()
    outer['files']={n:pin(b) for n,b in sorted(files.items()) if n!='bootstrap-package.json'};files['bootstrap-package.json']=encoded(outer)
    allowed=set(DEFAULTS)|{'stream-bridge-bundle.json','stream-bridge-package-source.json','bootstrap-package.json','README.txt'}
    if any(before[n]!=b for n,b in files.items() if n not in allowed):raise ValueError('Unrelated payload changed')
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,data in sorted(files.items()):
            entry=zipfile.ZipInfo(name,date_time=(2026,10,18,0,0,0));entry.external_attr=(0o100700 if name=='setup/steam-link-askpass.sh' else 0o100600)<<16
            z.writestr(entry,data,compress_type=zipfile.ZIP_DEFLATED,compresslevel=6)
    return out.getvalue(),True

def publish(channel):
    def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip()
    if git('branch','--show-current')!=channel or git('status','--porcelain'):raise RuntimeError('Publication requires a clean matching branch')
    path=ROOT/ARCHIVE;raw,changed=retarget(path.read_bytes(),channel)
    if not changed:print('Logging channel already matches '+channel);return
    path.write_bytes(raw);git('add',ARCHIVE);git('commit','-m','Set '+channel+' package logging default')
    archive=git('rev-parse','HEAD');identity=pin(raw)
    for name,prefix in [('get.ps1','    $sfnpRepository'),('get.sh','REPOSITORY_')]:
        path=ROOT/name;text=path.read_text()
        for key,value in [('Commit',archive),('Bytes',identity['bytes']),('Sha256',identity['sha256'])]:
            var=prefix+(key if name.endswith('ps1') else key.upper());value=str(value) if key=='Bytes' else "'"+value+"'"
            text,n=re.subn(r'(?m)^'+re.escape(var)+r' = .*',lambda m:var+' = '+value,text)
            if n!=1:raise RuntimeError('Bootstrap pin anchor changed')
        path.write_text(text)
    git('add','get.ps1','get.sh');git('commit','-m','Pin '+channel+' logging package');core=git('rev-parse','HEAD')
    for extension in ('ps1','sh'):
        path=ROOT/('get-preview.'+extension);text=path.read_text()
        text,n=re.subn(r'(?<=steam-link-native-passthrough/)[0-9a-f]{40}(?=/get\.)',core,text)
        if n!=1:raise RuntimeError('Alias revision anchor changed')
        text,n=re.subn(r"'[0-9a-f]{64}'","'"+pin((ROOT/('get.'+extension)).read_bytes())['sha256']+"'",text)
        if n!=1:raise RuntimeError('Alias hash anchor changed')
        path.write_text(text)
    subprocess.run(['python3','-B','tests/test_setup_bootstrap.py','--pwsh','pwsh'],cwd=ROOT,check=True)
    git('add','get-preview.ps1','get-preview.sh');git('commit','-m','Refresh '+channel+' logging bootstrap aliases')
    git('push','origin',channel)
    print('Published verified '+channel+' logging default; native and setup binaries unchanged')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--channel',required=True,choices=('main','dev'));parser.add_argument('--publish',action='store_true');args=parser.parse_args()
    if args.publish:publish(args.channel)
    else:
        _,changed=retarget((ROOT/ARCHIVE).read_bytes(),args.channel)
        print('Package logging channel '+('needs publication' if changed else 'matches')+' '+args.channel)
