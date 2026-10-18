#!/usr/bin/env python3
"""Actual Unity rasters -> lossy codec -> original-native fragment, offline only."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ORIGINAL_SHA = 'f28dd5d22a570cbee64057da03d0418b80aee88a8eeed988b2f776e8d9a513f2'
ROBUST_SHA = '44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49'
DUAL_SHA = '9ad1b56a20db039132ad6ec4557fe8ece3fff277316370bcefc9f70ec7aed367'
CASE = struct.Struct('<II36fII')


def require(ok, text):
    if not ok: raise ValueError(text)


def sha(raw): return hashlib.sha256(raw).hexdigest()


def f32(x): return struct.unpack('<f', struct.pack('<f', x))[0]


def inverse_native(rgb):
    """Inverse the exact pinned non-YCoCg native matrix; this is NOT raw RGB."""
    r, g, b = rgb
    red, blue = .468100011 / 1.57480001, .187299997 / 1.8556
    y = (g + blue * b + red * r) / (1 + blue + red)
    return [(r-y)/1.57480001+.501960814, y, (b-y)/1.8556+.501960814]


def decode_native(raw):
    cr, y, cb = raw
    return [y+1.57480001*(cr-.501960814), y-.187299997*(cb-.501960814)-.468100011*(cr-.501960814), y+1.8556*(cb-.501960814)]


def panel_coordinates(q, profile):
    q = [min(f32(1-.5/2048), max(f32(.5/2048), f32(v))) for v in q]
    x, y, w, h = profile['scene']; cw, ch = profile['canvas']
    color = [f32(f32(f32(w/cw)*q[0])+f32(x/cw)), f32(f32(f32(h/ch)*q[1])+f32(y/ch))]
    size = profile['coverage']['size'][0]; rows = profile['coverage']['tile_rows']
    px, py = [f32(f32(v*size)-.5) for v in q]
    tile = min(3, math.floor(max(0, py)/rows)); local = f32(py-tile*rows)
    rx, ry = (local, f32(size-1-px)) if profile['coverage']['rotation_quarter_turns'][tile] else (px, local)
    ox, oy = profile['coverage']['data_origins'][tile]
    mask = [f32(f32(f32(ox+rx)+.5)/cw), f32(f32(f32(oy+ry)+.5)/ch)]
    return color, mask


def chart(q, eye): return [f32(q[0]), f32((.25 if eye == 0 else .75)+f32(q[1]*.25))]


def codec_args(codec, qp, width, output):
    require(codec in ('libx264', 'libx265') and qp in (23, 32) and width in (576, 1152), 'bounded codec matrix')
    args = ['ffmpeg','-v','error','-y','-threads','1','-f','rawvideo','-pixel_format','rgb24','-video_size',f'{width}x{width}','-framerate','90','-i','pipe:0',
            '-vf','scale=in_range=full:out_range=limited:out_color_matrix=bt709','-frames:v','1','-c:v',codec,'-preset','ultrafast','-threads','1','-qp',str(qp),'-pix_fmt','yuv420p']
    if codec == 'libx265': args += ['-x265-params','pools=1:frame-threads=1:log-level=error']
    return args + ['-color_range','tv','-colorspace','bt709',str(output)]


def command(args, *, env=None, payload=None, timeout=180):
    p = subprocess.Popen([str(x) for x in args], stdin=subprocess.PIPE if payload is not None else subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, start_new_session=True)
    try: stdout, stderr = p.communicate(payload, timeout=timeout)
    except BaseException:
        if p.poll() is None: os.killpg(p.pid, signal.SIGKILL)
        p.communicate(); raise
    return p.returncode, stdout, stderr


def checked(args, steps, **kwargs):
    code, out, err = command(args, **kwargs)
    steps.append({'command':[str(x) for x in args], 'returncode':code,'stderr':err.decode(errors='replace')[-65536:]})
    require(code == 0, 'subprocess failed: '+str(args[0])+' '+err.decode(errors='replace')[-1000:])
    return out


def bilinear(image, uv):
    import numpy as np
    height, width = image.shape[:2]; x, y = uv[0]*width-.5, uv[1]*height-.5
    ix, iy = math.floor(x), math.floor(y); fx, fy = x-ix, y-iy
    values = np.array([image[min(height-1,max(0,iy+dy)),min(width-1,max(0,ix+dx))] for dy in (0,1) for dx in (0,1)], dtype=np.float64)
    weights = [(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy]
    return np.dot(weights, values), values.max(axis=0)-values.min(axis=0)


def role(source):
    a = source[3]
    if a < .01 and sum(source[:3]) > .05: return 4
    for i, v in enumerate((.25,.5,.75),1):
        if abs(a-v) < .02: return i
    return 0


def guard_values(image, profile):
    g = profile['guard']; width, height = profile['canvas']; result=[]
    for i, code in enumerate(g['codes']):
        origin = g['origins'][i] if 'origins' in g else [g['origin'][a]+g['stride'][a]*i for a in (0,1)]
        uv = [(origin[a]+g['size'][a]/2)/(width if a==0 else height) for a in (0,1)]
        rgb, _ = bilinear(image,uv); value=inverse_native(rgb)[1]*255
        result.append({'index':i,'expected':code,'native_g_code':value,'error':value-code})
    return {'values':result,'accepted':all(abs(x['error'])<=g['code_tolerance'] for x in result),'max_error':max(abs(x['error']) for x in result)}


def make_cases(probes, sources, decoded, profile):
    cases=[]
    for p in probes:
        eye = {'left':0,'right':1}[p['eye']]; q=[f32(x) for x in p['source_q']]
        require(all(.04<=x<=.96 for x in q), 'test witnesses must avoid original aperture falloff')
        source,_ = bilinear(sources[eye],q)
        require(max(abs(source[i]-p['source_rgba'][i]) for i in range(4))<1.1/255, 'actual source PNG orientation/probe mismatch')
        qc, qa = panel_coordinates(q,profile); rgb, rgb_span=bilinear(decoded[eye],qc); mask, mask_span=bilinear(decoded[eye],qa)
        a = min(1,max(0,inverse_native(mask)[1])); expected=[*rgb,a]
        # The scalar luma range is bounded by the sum of component ranges with
        # positive luma coefficients. This only bounds hardware interpolation.
        alpha_span = inverse_native(mask_span)[1]
        span = [*rgb_span,alpha_span]
        y=.25 if eye==0 else .75; u=[*chart(q,eye),.625,.875]
        native=[*u,.1,y+.15*.25,.6,y+.85*.25,0,y,1,y+.25,.12,.14,0,0,1,1,1,1]
        require(len(native)==20,'native metadata size')
        cases.append({'eye':eye,'label':p['label'],'q':q,'expected':expected,'source':source.tolist(),'span':span,'native':native,'role':role(source)})
    require(len(cases)<=128 and {c['eye'] for c in cases}=={0,1}, 'bounded paired-eye inputs')
    return cases


def packed_texture(decoded, width, output):
    import numpy as np
    # Treat codec output as RGB only at this boundary, then inverse the native
    # matrix for every texel. Passing decoded RGB directly as B9 would be wrong.
    raw=np.empty((width*4,width,4),dtype='<f4'); raw[:]=[*inverse_native((.8,.05,.1)),1]
    for eye, rgb in enumerate(decoded):
        red,blue=.468100011/1.57480001,.187299997/1.8556
        y=(rgb[:,:,1]+blue*rgb[:,:,2]+red*rgb[:,:,0])/(1+red+blue)
        start=(1 if eye==0 else 3)*width; target=raw[start:start+width]
        target[:,:,0]=(rgb[:,:,0]-y)/1.57480001+.501960814;target[:,:,1]=y;target[:,:,2]=(rgb[:,:,2]-y)/1.8556+.501960814;target[:,:,3]=1
    raw.tofile(output)


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('producer','original','candidate','historical-dual','record'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--driver',choices=('intel','software'),default='intel');args=p.parse_args(argv)
    report={'schema':'actual-producer-lossy-native-stereo-v1','status':'FAILED','steps':[],'conditions':[],
            'frame_contact':False,'native_frame_eye_association_proven':False,'steam_link_codec_reproduced':False,'performance_measured':False}
    code=1; cache=None
    try:
        from PIL import Image
        import numpy as np
        require(not args.record.exists(),'exclusive output receipt');args.record.parent.mkdir(parents=True,exist_ok=True)
        inputs=json.loads((args.producer/'codec-inputs.json').read_text());probes=json.loads((args.producer/inputs['probe_file']).read_text())
        require(inputs['scope']=='ACTUAL_COMPILED_UNITY_PRODUCER_NOT_IDEAL_REPAINT' and probes['passed'] is True and probes['carrier']==4040 and probes['actual_samples']==8,'actual producer evidence required')
        report['producer_manifest_sha256']=sha((args.producer/'codec-inputs.json').read_bytes());report['producer_bank_sha256']=inputs['bank_sha256']
        profiles={r:json.loads((ROOT/f'docs/protocols/scene-contribution-atlas-v{r}.json').read_text()) for r in (2,3)}
        require(sha((ROOT/'docs/protocols/scene-contribution-atlas-v3.json').read_bytes())==inputs['profile_sha256'],'frozen source/consumer profile differs')
        images={};report['inputs']={}
        for name, pin in inputs['images'].items():
            path=args.producer/name;raw=path.read_bytes();require(len(raw)==pin['bytes'] and sha(raw)==pin['sha256'],'producer image pin mismatch: '+name)
            image=Image.open(path);require(image.size==(4040,4040),'actual compiled carrier extent');images[name]=image.convert('RGBA');report['inputs'][name]=pin
        for path, expected in ((args.original,ORIGINAL_SHA),(args.candidate,ROBUST_SHA),(args.historical_dual,DUAL_SHA)):
            require(sha(path.read_bytes())==expected,'private shader pin mismatch');report['inputs'][str(path)]={'sha256':expected,'bytes':path.stat().st_size}
        sources=[np.asarray(images[eye+'-source.png'],dtype=np.float32)/255 for eye in ('left','right')]
        for rev in (2,3):
            for eye in ('left','right'):
                values=guard_values(np.asarray(images[f'{eye}-v{rev}-carrier.png'],dtype=np.float32)[:,:,:3]/255,profiles[rev]);require(values['max_error']<1.1,'actual PNG header orientation/code mismatch')
        with tempfile.TemporaryDirectory(prefix='sfnp-real-codec-') as tmp:
            cache=Path(tmp);borrowed=(ROOT/'tests/native-fragment-guarded-presenter-gpu/fixture.cpp').read_text()
            borrowed=borrowed.replace('int main(int argc,char** argv){','int inherited_guard_main(int argc,char** argv){').replace('#include "../native-fragment-ssa-gpu/fixture.cpp"','#include "'+str(ROOT/'tests/native-fragment-ssa-gpu/fixture.cpp')+'"').replace('constexpr unsigned SourceWidth=688,SourceHeight=548;','constexpr unsigned SourceWidth=STREAM_CHART_WIDTH,SourceHeight=STREAM_CHART_WIDTH*4;')
            (cache/'borrowed_guard_fixture.h').write_text(borrowed)
            vertex=cache/'vertex.spv';checked(['glslc','--target-env=vulkan1.1',ROOT/'tests/native-fragment-ssa-gpu/fixture.vert','-o',vertex],report['steps'])
            for module in (args.original,args.candidate,args.historical_dual):checked(['spirv-val','--target-env','vulkan1.1',module],report['steps'])
            binaries={}
            for width in (1152,576):
                binary=cache/f'fixture-{width}';checked(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',f'-DSTREAM_CHART_WIDTH={width}','-I',cache,HERE/'fixture.cpp','-lvulkan','-o',binary],report['steps']);binaries[width]=binary
            matrix=[(3,width,codec,qp) for width in (1152,576) for codec in ('libx264','libx265') for qp in (23,32)] + [(2,1152,'libx265',23)]
            codec_dir=args.record.parent/'codec';codec_dir.mkdir()
            for rev,width,codec,qp in matrix:
                label=f'v{rev}-{width}-{codec}-qp{qp}';decoded=[];condition={'label':label,'revision':rev,'chart_extent':[width,width],'source_extent':[width,width*4],'codec':codec,'qp':qp,'guards':[],'eye_images':[]}
                for eye in ('left','right'):
                    small=images[f'{eye}-v{rev}-carrier.png'].convert('RGB').resize((width,width),Image.Resampling.BILINEAR)
                    stream=codec_dir/f'{label}-{eye}.mkv';args_encode=codec_args(codec,qp,width,stream);checked(args_encode,report['steps'],payload=small.tobytes())
                    raw=checked(['ffmpeg','-v','error','-threads','1','-i',stream,'-vf','scale=in_range=limited:out_range=full:in_color_matrix=bt709','-frames:v','1','-f','rawvideo','-pix_fmt','rgb24','pipe:1'],report['steps'])
                    require(len(raw)==width*width*3,'complete decoded RGB extent');rgb=np.frombuffer(raw,dtype=np.uint8).reshape((width,width,3)).astype(np.float64)/255;decoded.append(rgb)
                    condition['guards'].append(guard_values(rgb,profiles[rev]));condition['eye_images'].append({'eye':eye,'bitstream_sha256':sha(stream.read_bytes()),'bitstream_bytes':stream.stat().st_size,'decoded_rgb_sha256':sha(raw)})
                    Image.fromarray(np.frombuffer(raw,dtype=np.uint8).reshape((width,width,3))).save(codec_dir/f'{label}-{eye}-decoded.png')
                case_list=make_cases(probes['probes'],sources,decoded,profiles[rev]);casefile=cache/'cases.bin'
                casefile.write_bytes(b'SFNPSTR1'+struct.pack('<I',len(case_list))+b''.join(CASE.pack(c['eye'],0x02038018,*c['native'],*c['expected'],*c['source'],*c['q'],0,0,*c['span'],c['role'],1) for c in case_list))
                texture=cache/'decoded-packed.rgba32f';packed_texture(decoded,width,texture)
                env=dict(os.environ,VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/'+('intel_icd.json' if args.driver=='intel' else 'lvp_icd.json'),XDG_CACHE_HOME=str(cache/'driver-cache'),VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT')
                settings=cache/'settings';settings.mkdir(exist_ok=True);validation=settings/'validation.log';validation.unlink(missing_ok=True)
                (settings/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\nkhronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG\nkhronos_validation.report_flags = error,warn\nkhronos_validation.log_filename = '+str(validation)+'\n');env['VK_LAYER_SETTINGS_PATH']=str(settings)
                candidate=args.candidate if rev==3 else args.historical_dual
                rc,out,err=command([binaries[width],vertex,args.original,candidate,texture,casefile],env=env)
                condition['returncode']=rc;condition['stderr']=err.decode(errors='replace');condition['validation_log']=validation.read_text(errors='replace') if validation.exists() else ''
                require('Validation Error' not in condition['stderr']+condition['validation_log'] and 'SYNC-HAZARD' not in condition['validation_log'],'Vulkan/sync failure')
                condition['gpu_stdout']=out.decode(errors='replace')[-65536:];report['conditions'].append(condition)
                require(rc in (0,2),'native GPU fixture failed: '+condition['stderr'][-1500:]);condition['gpu']=json.loads(out)
                if rev==3:require(rc==0 and condition['gpu']['stereo_criterion_pass'] is True,'valid actual v3 stream failed coupled-eye criterion: '+label)
                else:require(rc==2 and condition['gpu']['stereo_criterion_pass'] is False,'known bad v2 input did not fail coupled-eye criterion')
                condition['expected_outcome']='valid_stereo' if rev==3 else 'known_bug_rejected_by_stereo_criterion'
                texture.unlink();casefile.unlink();print(json.dumps({'condition':label,'status':'PASS','stereo':condition['gpu']['eye_matches']}),flush=True)
            report['status']='PASS';code=0
    except Exception as error:
        report['error']=str(error);print(json.dumps({'status':'FAILED','error':str(error)}),file=sys.stderr)
    finally:
        s=importlib.util.spec_from_file_location('run_retention','/home/claw/gloomhaven_vr/scripts/town_run_retention.py');ret=importlib.util.module_from_spec(s);s.loader.exec_module(ret)
        report['cleanup']=ret.cleanup_completed_run('actual-producer-lossy-native-stereo',code,args.record,ROOT,process_reaped=True)
        report['private_cache_removed']=cache is None or not cache.exists();args.record.write_text(json.dumps(report,indent=2)+'\n')
    return code


if __name__=='__main__':raise SystemExit(main())
