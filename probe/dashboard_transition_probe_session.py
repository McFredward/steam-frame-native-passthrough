#!/usr/bin/env python3
"""Prepare/status/collect only. Never install, restart, or edit a Steam setting."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import sys
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def no_symlink_ancestors(path):
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('absolute non-traversing path required')
    for ancestor in reversed(path.parents):
        if ancestor.is_symlink() or not ancestor.is_dir():
            raise ValueError('existing non-symlink parent directories required')


def owned(path, maximum=2*1024*1024):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or not 0 < info.st_size <= maximum:
            raise ValueError('bounded, regular, owned, non-linked input required')
        data = b''
        while len(data) < info.st_size:
            part = os.read(fd, info.st_size-len(data))
            if not part:
                raise ValueError('incomplete input')
            data += part
        return data
    finally:
        os.close(fd)


def write(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(data)
    except Exception:
        raise


def checked_run(path):
    no_symlink_ancestors(path)
    if not path.is_absolute() or path.is_symlink():
        raise ValueError('explicit absolute run directory required')
    info = path.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError('owned private0700 run required')
    receipt = json.loads(owned(path/'dashboard-transition-prepared.json'))
    for name, digest in receipt['files'].items():
        if hashlib.sha256(owned(path/name)).hexdigest() != digest:
            raise ValueError('prepared artifact changed: '+name)
    return receipt


def prepare(run, library, shipping, host_fixture=False):
    no_symlink_ancestors(run)
    if not run.is_absolute() or run.exists() or run.is_symlink():
        raise ValueError('new absolute run directory required')
    code = owned(library)
    if code[:4] != b'\x7fELF' or code[4] != 2 or code[5] != 1:
        raise ValueError('ELF64 little-endian library required')
    if b'SFNP_DASHBOARD_TRANSITION_PROBE/1' not in code:
        raise ValueError('isolated dashboard diagnostic build marker required')
    machine = struct.unpack_from('<H', code, 18)[0]
    if machine != 183 and not (host_fixture and machine == 62):
        raise ValueError('native AArch64 library required; host fixtures are explicitly separate')
    source = owned(shipping)
    # Runtime status/phase/collect remain standalone; only offline preparation
    # imports the source transformer and its repository-owned dependencies.
    spec = importlib.util.spec_from_file_location('dashboard_sparse_transform', HERE/'dashboard_transition_probe_spirv.py')
    transform = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transform)
    probe, manifest = transform.instrument(source)
    run.mkdir(mode=0o700)
    capture = os.urandom(16).hex()
    files = {
        'libVkLayer_dashboard_transition_probe.so': code,
        'scene-presenter.spv': source,
        'scene-presenter.enable': b'GHVR_SCENE_PRESENTER/1\n',
        'dashboard-transition.spv': probe,
        'dashboard-transition.enable': b'SFNP_DASHBOARD_TRANSITION/1\n',
        'session.conf': ('GHVR_PIXEL_SNAPSHOT/1\n'+capture+'\n'+str(run)+'\n').encode(),
    }
    layer = {'file_format_version':'1.2.0','layer':{
        'name':'VK_LAYER_GHVR_pixel_snapshot','type':'GLOBAL',
        'library_path':str(run/'libVkLayer_dashboard_transition_probe.so'),
        'api_version':'1.3.0','implementation_version':'1',
        'description':'Temporary bounded dashboard transition diagnostic; not a second active receiver',
        'functions':{'vkGetInstanceProcAddr':'frame_pixel_vkGetInstanceProcAddr',
                     'vkGetDeviceProcAddr':'frame_pixel_vkGetDeviceProcAddr',
                     'vkNegotiateLoaderLayerInterfaceVersion':'frame_pixel_vkNegotiateLoaderLayerInterfaceVersion'},
        'disable_environment':{'GHVR_PIXEL_SNAPSHOT_DISABLE':'1'}}}
    files['VkLayer_dashboard_transition_probe.json'] = (json.dumps(layer,indent=2)+'\n').encode()
    for name, data in files.items():
        write(run/name, data)
    record = {'schema':'sfnp-dashboard-transition-prepared-v1','capture_id':capture,
        'created_unix_ns':time.time_ns(),'machine':machine,'host_fixture':host_fixture,
        'files':{name:hashlib.sha256(data).hexdigest() for name,data in files.items()},
        'shader':manifest,'installed':False,'restart_performed':False,'automatic_expiry':False,
        'required_environment':{'GHVR_PIXEL_SNAPSHOT_CONFIG':str(run/'session.conf')},
        'coexistence':{'same_loader_namespace':'VK_LAYER_GHVR_pixel_snapshot',
            'two_active_receivers_allowed':False,
            'require_before_future_attachment':'Snapshot and suspend the existing public receiver activation; keep preferences and restore the exact activation after manual collection. Never replace a mapped library.',
            'installation_authorized':False},
        'hardware_readiness':'Requires source review, AArch64 provider/load preflight, and explicit future installation/restart permission.'}
    write(run/'dashboard-transition-prepared.json',(json.dumps(record,indent=2)+'\n').encode())
    return record


def status(run):
    receipt = checked_run(run)
    paths = sorted(run.glob('dashboard-transition-status-[0-9][0-9].json'))
    if len(paths)>24:
        raise ValueError('status output budget exceeded')
    terminal=run/'dashboard-transition-terminal.json'
    latest = json.loads(owned(terminal,1024)) if terminal.exists() else (json.loads(owned(paths[-1],1024)) if paths else None)
    total=sum(p.stat().st_size for p in paths)
    return {'schema':'sfnp-dashboard-transition-session-status-v1','capture_id':receipt['capture_id'],
        'latest':latest,'status_output_bytes':total,'installed_by_helper':False,'hardware_success_claim':False}


def phase(run, value):
    checked_run(run)
    for before in range(value):
        if not (run/('dashboard-transition-phase-%u.json'%before)).is_file():
            raise ValueError('manual phases must be issued0,1,2,3 exactly once')
    if value:
        state=status(run)['latest']
        if not state or state['queued_phase'] != value-1:
            raise ValueError('previous phase must have reached the actual native queue')
    context = ('closed_before','dashboard_open','closed_after','disable_and_collect')[value]
    # One immutable command receipt per deliberate phase; no timer and no reset.
    write(run/('dashboard-transition-phase-%u.json'%value),json.dumps({
        'phase':value,'context':context,'unix_ns':time.time_ns(),
        'menu_visible_claim':'manual operator context, independently correlate systemui log'}).encode())
    temporary=run/'dashboard-transition.phase.next'
    write(temporary,('%u\n'%value).encode())
    os.replace(temporary,run/'dashboard-transition.phase')
    return status(run)


def collect(run):
    result=status(run)
    if not result['latest'] or not result['latest']['collected']:
        raise ValueError('not collected: require explicit phase3, actual queue submission and completed owned fence')
    raw=owned(run/'dashboard-transition-records.bin',1600)
    if len(raw)!=1600:
        raise ValueError('exact sparse buffer required')
    words=struct.unpack('<400I',raw)
    slots=[]
    for index in range(6):
        record=words[16+64*index:16+64*(index+1)]
        valid=record[0]==0x44545031 and record[1]==1 and record[2]==index//2 and record[3]==index%2
        slots.append({'phase':index//2,'consumed_selector':index%2,'present':valid,
            'branch_kind':record[4] if valid else None,'pipeline_tag':record[6] if valid else None,
            'r1_ordered_span_valid':bool(record[63]) if valid else None,
            'guard_existing_reads_observed':sum(v!=0x7fc00001 for v in record[32:44]) if valid else 0})
    routes=owned(run/'dashboard-transition-routes.bin',16384)
    unknown=owned(run/'dashboard-transition-unknown-routes.bin',24)
    route_schema=struct.Struct('<Q4I32s32s3Q')
    if len(routes)!=64*route_schema.size:
        raise ValueError('exact bounded route schema required')
    decoded_routes=[]
    for index in range(64):
        handle,tag,reason,retired,reserved,vs,ps,*counts=route_schema.unpack_from(routes,index*route_schema.size)
        if handle:
            decoded_routes.append({'route_index':index,'native_handle':handle,'companion_tag':tag,
                'native_admission_reason':reason,'retired':bool(retired),'vertex_sha256':vs.hex(),
                'fragment_sha256':ps.hex(),'submission_attempts_using_route_by_phase':counts})
    result.update(slots=slots,records_sha256=hashlib.sha256(raw).hexdigest(),
        route_bytes=len(routes),routes=decoded_routes,unknown_route_counts=struct.unpack('<3Q',unknown),
        sparse_context_complete=all(s['present'] and s['r1_ordered_span_valid'] for s in slots),
        full_pipeline_census=False,physical_eye_proven=False,final_visible_composition_proven=False,
        independent_overlay_transport_proven=False,
        interpretation='Missing slots or invalid R1 are incomplete measurements, never proof of a guard rejection. Routes count actual submitted recorded command uses per phase; no claim that one sample represents all scene draws.')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=('prepare','status','phase','collect'))
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--library',type=Path)
    p.add_argument('--shipping',type=Path)
    p.add_argument('--host-fixture',action='store_true')
    p.add_argument('--value',type=int,choices=range(4))
    a=p.parse_args()
    if a.action=='prepare':
        if not a.library or not a.shipping:p.error('prepare needs --library and --shipping')
        result=prepare(a.run,a.library,a.shipping,a.host_fixture)
    elif a.action=='phase':
        if a.value is None:p.error('phase needs --value')
        result=phase(a.run,a.value)
    else:result=globals()[a.action](a.run)
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
