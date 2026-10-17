#!/usr/bin/env python3
"""Owned Floatatlas scene-contribution proof against actual original PS oracles."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import native_fragment_scene_presenter as scene
import pixel_uniform_mapping as ref

F=ref.f32
WIDTH,HEIGHT=2112,2752
FULL_PROFILE=scene.load_profile(scene.SPEC_V2)
LEGACY_PROFILE=scene.load_profile()
CHARTS=[[F(x) for x in r] for r in [[0,.25,1,.5],[0,0,1,1],[0,.75,1,1],[.13,.29,.81,.73]]]
BG=[F(x) for x in [.11,.17,.23,1]]
CASE=struct.Struct('<II32fIIiIII16fI')


def demand(value,message):
    if not value:raise ValueError(message)


def invoke(command,env=None,timeout=120):
    p=subprocess.run(command,env=env,capture_output=True,text=True,timeout=timeout)
    return {'command':[str(x) for x in command],'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}


def encoded(rgb,flags):
    r,g,b=rgb[:3]
    if (flags>>16)&4:
        y=(r+b+2*g)/4;return [F((r-b)/4+.5),F(y),F((g-y)/2+.5),1.]
    y=(g+.187299997*b/1.8556+.468100011*r/1.57480001)/(1+.187299997/1.8556+.468100011/1.57480001)
    return [F((r-y)/1.57480001+.501960814),F(y),F((b-y)/1.8556+.501960814),1.]


def source_pixel(x,y,c,p):
    p=FULL_PROFILE if c.get('profile_revision',1)==2 else LEGACY_PROFILE
    chart=CHARTS[c['chart']]
    ax=(((x+.5)/WIDTH-chart[0])/(chart[2]-chart[0]))*p['canvas'][0]
    ay=(((y+.5)/HEIGHT-chart[1])/(chart[3]-chart[1]))*p['canvas'][1]
    out=[F(.24),F(.53),F(.69),1.]
    sx,sy,sw,sh=p['scene']
    if sx<=ax<sx+sw and sy<=ay<sy+sh:out=encoded(c['body'],c['flags'])
    for tile,(ox,oy) in enumerate(p['coverage']['data_origins']):
        rotation=p['coverage'].get('rotation_quarter_turns',[0]*4)[tile]
        size=p['coverage']['size'][0];rows=p['coverage']['tile_rows']
        w,h=(rows,size) if rotation else (size,rows)
        if ox-16<=ax<ox+w+16 and oy-16<=ay<oy+h+16:
            gx=(size-(ay-oy)-.5 if rotation else ax-ox-.5)
            gy=(ax-ox-.5 if rotation else ay-oy-.5)+tile*rows
            gx=max(0.,min(size-1.,gx));gy=max(0.,min(size-1.,gy))
            a={0:0.,1:1.,3:-.2,4:1.2,6:.25,7:.5}.get(c['pattern'])
            if c['pattern']==5:a=0. if gx<(size-1)/2 else 1.
            if a is None:a=.17+.51*gx/(size-1)+.21*gy/(size-1)
            out=[F(.31),F(a),F(.68),1.];break
    if c['mode']==7:out=[F(.21),F(.5),F(.72),1.]
    if c['mode'] not in (5,7):
        gx,gy=ax,ay
        if c['mode']==2:gx-=80
        if c['mode']==3:gx=2112-ax
        if c['mode']==4:gx=64+(ax-64)*2;gy=2400+(ay-2400)*2
        for i,code in enumerate(p['guard']['codes']):
                ox,oy=[p['guard']['origin'][axis]+i*p['guard']['stride'][axis] for axis in (0,1)]
                if ox<=gx<ox+32 and oy<=gy<oy+32:
                    if c['mode']==1:code^=0x5a
                    if c['mode']==8 and i==c['cell']:code+=c['delta']
                    out=[F(.31),F(code/255),F(.68),1.];break
    return out


def sample(uv,c,profile):
    # Independent clamp/linear Floatatlas oracle, before native RGB decode.
    xy=[float(uv[i])*(WIDTH if i==0 else HEIGHT)-.5 for i in (0,1)]
    lo=[math.floor(x) for x in xy];fraction=[xy[i]-lo[i] for i in (0,1)]
    result=[0.]*3
    for dy in (0,1):
        for dx in (0,1):
            x=max(0,min(WIDTH-1,lo[0]+dx));y=max(0,min(HEIGHT-1,lo[1]+dy))
            w=(fraction[0] if dx else 1-fraction[0])*(fraction[1] if dy else 1-fraction[1])
            value=source_pixel(x,y,c,profile)
            for i in range(3):result[i]+=value[i]*w
    return [F(x) for x in result]


def tiled_coordinate(q,profile=None):
    profile=profile or LEGACY_PROFILE
    q=[min(F(1-.5/2048),max(F(.5/2048),x)) for x in q]
    size=profile['coverage']['size'][0];rows=profile['coverage']['tile_rows']
    px,py=[ref.sub(ref.mul(x,size),.5) for x in q]
    tile=min(3,math.floor(max(py,0)/rows))
    ox,oy=profile['coverage']['data_origins'][tile]
    py=ref.sub(py,tile*rows)
    if profile['coverage'].get('rotation_quarter_turns',[0]*4)[tile]:px,py=py,ref.sub(size-1,px)
    return [ref.div(ref.add(ref.add(ox,px),.5),2112),
            ref.div(ref.add(ref.add(oy,py),.5),2752)] if profile['revision']==1 else [
                ref.div(ref.add(ref.add(ox,px),.5),profile['canvas'][0]),
                ref.div(ref.add(ref.add(oy,py),.5),profile['canvas'][1])]


def color_coordinate(q,profile=None):
    profile=profile or LEGACY_PROFILE
    q=[min(F(1-.5/2048),max(F(.5/2048),x)) for x in q]
    # Match only frozen construction coefficients, not the shader's SSA graph.
    sx,sy,sw,sh=profile['scene'];w,h=profile['canvas']
    return [ref.add(ref.mul(F(sw/w),q[0]),F(sx/w)),ref.add(ref.mul(F(sh/h),q[1]),F(sy/h))]


def native_panel(q,c,p):
    layout=ref.lookup_reference(c['r0'],c['r1'],c['patch'][:2],c['flags'],q)
    first,second=[sample(layout[k],c,p) for k in ['first_packed_lookup','second_packed_lookup']]
    # Owned SFLOAT atlas includes deliberate negative/over-one G values. The
    # existing normalized-input reference stays strict; this local linear RGB
    # decoder is checked independently against the actual original GPU shader.
    def decode(v):
        r,g,b=v
        if (c['flags']>>16)&4:
            co,cg=ref.sub(ref.mul(2.,r),1.),ref.sub(ref.mul(2.,b),1.)
            return [ref.sub(ref.add(g,co),cg),ref.add(g,cg),ref.sub(ref.sub(g,co),cg)]
        u,v=ref.sub(b,F(.501960814)),ref.sub(r,F(.501960814))
        return [ref.add(g,ref.mul(F(1.57480001),v)),
                ref.add(ref.add(g,ref.mul(F(-.187299997),u)),ref.mul(F(-.468100011),v)),
                ref.add(g,ref.mul(F(1.8556),u))]
    native=layout;w=native['second_sample_weight']
    a,b=decode(first),decode(second)
    native['blended_rgb']=[ref.add(ref.mul(b[i],w),ref.mul(a[i],ref.sub(1.,w))) for i in range(3)]
    raw=ref.add(ref.mul(first[1],ref.sub(1.,w)),ref.mul(second[1],w))
    return native,raw


def cases(profile):
    result=[]
    def add(name,q=(.5,.5),chart=1,eye=0,branch=1,pattern=2,mode=0,cell=0,delta=0,valid=True,slot=0,r1=None,u=None,r0=None,body=None):
        r1=CHARTS[chart] if r1 is None else [struct.unpack('<f',struct.pack('<f',v))[0] for v in r1]
        raster=lambda point:[ref.add(r1[i],ref.mul(ref.sub(r1[i+2],r1[i]),F(point[i]))) for i in (0,1)]+[F(.625),F(.875)]
        body=[F(x) for x in (body or [.35,.65,.25,.375])]
        c={'name':name,'chart':chart,'eye':eye,'flags':0x02018018|((branch&1)<<17)|((branch&2)<<17),
           'profile_revision':profile['revision'],
           'pattern':pattern,'mode':mode,'cell':cell,'delta':delta,'valid':valid,'slot':slot,'r1':r1,'u':raster(q) if u is None else u,
           'patch':[F(.12),F(.14),0.,0.],'tint':[F(.73),F(.59),F(.91),1.],'body':body}
        c['r0']=(raster([.1,.15])[:2]+raster([.6,.85])[:2] if valid else [0.,0.,1.,1.]) if r0 is None else r0
        if valid:
            q=[ref.div(ref.sub(c['u'][i],r1[i]),ref.sub(r1[i+2],r1[i])) for i in (0,1)]
            qc,qa=color_coordinate(q,profile),tiled_coordinate(q,profile)
            color,_=native_panel(qc,c,profile);mask,raw=native_panel(qa,c,profile)
            aperture=ref.lookup_reference(c['r0'],r1,c['patch'][:2],c['flags'],q)['outer_attenuation']
            expected=[ref.mul(ref.mul(color['blended_rgb'][i],aperture),c['tint'][i]) for i in range(3)]+[ref.mul(min(1.,max(0.,raw)),aperture)]
            c.update(q=q,qc=qc,qa=qa,expected=expected,raw_g=raw,original_aperture=aperture,
                     color_aperture=color['outer_attenuation'],mask_aperture=mask['outer_attenuation'],
                     color_weight=color['second_sample_weight'],mask_weight=mask['second_sample_weight'],
                     color_u=raster(qc),mask_u=raster(qa))
        else:c.update(expected=[0.]*4,color_aperture=0.,mask_aperture=0.,original_aperture=0.,raw_g=0.,
                      color_weight=0.,mask_weight=0.,color_u=[0.]*4,mask_u=[0.]*4,qa=[0.,0.])
        if (c['flags']>>16)&4:c['inverse_g']=[.25,.5,.25]
        else:
            red,blue=F(.468100011)/F(1.57480001),F(.187299997)/F(1.8556);green=1/(1+red+blue)
            c['inverse_g']=[F(red*green),F(green),F(blue*green)]
        result.append(c)
    add('ON');add('OFF_same_command',mode=5,valid=False);add('ON_same_command_recovery')
    for cell in range(12):
        for delta in [-2,2,-3,3]:add('guard_'+str(cell)+'_'+str(delta),mode=8,cell=cell,delta=delta,valid=abs(delta)<=2)
    for mode,name in [(1,'wrong'),(2,'shifted'),(3,'mirrored'),(4,'scaled'),(5,'erased'),(7,'plain')]:add(name,mode=mode,valid=False)
    for pattern in [0,1,3,4]:add('coverage_pattern_'+str(pattern),pattern=pattern)
    for y in [0,.25,.5,.75,1]:
        for offset in [-.5/1024,0,.5/1024]:
            point=max(0.,min(1.,y+offset))
            for x in [0.,.37,1.]:add('global_seam_'+str((x,point)),q=(x,point),mode=9)
    for chart in range(4):
        for eye in [0,1]:
            for branch in range(4):add('native_routes_'+str((chart,eye,branch)),chart=chart,eye=eye,branch=branch,q=(.41,.62))
    add('partial_aperture',q=(.99,.447))
    add('other_image_invalid',slot=1,mode=7,valid=False);add('other_image_valid',slot=1)
    add('first_image_changed_body',body=[.8,.3,.1,.625])
    add('invalid_R1_zero',r1=[0.,0.,0.,1.],u=[.5,.5,.625,.875],valid=False)
    add('invalid_R1_reverse',r1=[.8,0.,.2,1.],u=[.5,.5,.625,.875],valid=False)
    add('invalid_R1_NaN',r1=[float('nan'),0.,1.,1.],u=[.5,.5,.625,.875],valid=False)
    add('invalid_u_outside',u=[-1.,.5,.625,.875],valid=False)
    if profile['revision']==2:
        for pattern in (6,7):add('full_constant_'+str(pattern),pattern=pattern)
        for offset in range(-4,5):add('full_edge_'+str(offset),q=(.5+offset/4096,.13),pattern=5,mode=9)
    demand(len(result)<=256,'bounded scene cases')
    return result


def pack(cases,coordinates=False):
    records=[]
    for c in cases:
        expected=[*c['qa'],c['raw_g'],c['original_aperture']] if coordinates and c['valid'] else c['expected']
        values=c['u']+c['r0']+c['r1']+c['patch']+c['tint']+expected+c['body']+BG
        extra=c['color_u']+c['mask_u']+[c['color_aperture'],c['mask_aperture'],c['original_aperture']]+c['inverse_g']+[c['color_weight'],c['mask_weight']]
        records.append(CASE.pack(c['eye'],c['flags'],*values,c['mode'],c['cell'],c['delta'],c['chart'],int(c['valid']),c['slot'],*extra,c['pattern']+(256 if c.get('profile_revision',1)==2 else 0)))
    return b'SCENEAT1'+struct.pack('<I',len(records))+b''.join(records)


def mutated(binary,record,mode):
    h,ins=scene.parse_module(binary);ids=record['new_ssa_ids'];out=[];count=0;bound=h[3]
    for item in ins:
        op,a=item.opcode,item.operands
        if mode=='multiply_E_by_A' and op==83 and a[1]==ids['scene_contribution_rgb']:
            item=scene.ssa.Instruction(142,(30,a[1],a[2],ids['clamped_coverage']));count+=1
        if mode=='wrong_bottom_tile' and op==169 and a[1]==ids['mask_origin_y']:
            item=scene.ssa.Instruction(83,(8,a[1],a[4]));count+=1
        if mode=='wrong_R1_chart' and op==12 and a[1]==ids['scene_global_q']:
            item=scene.ssa.Instruction(12,(*a[:4],538,*a[5:]));count+=1
        if mode=='shared_weight' and op in (131,133) and ids['mask_weight'] in a[2:]:
            item=scene.ssa.Instruction(op,(*a[:2],*(ids['color_weight'] if x==ids['mask_weight'] else x for x in a[2:])));count+=1
        if mode=='coordinates' and op==62 and a==(504,ids['output']):
            out.append(scene.ssa.Instruction(80,(9,bound,ids['mapped_qa'],ids['raw_component_g'],ids['original_aperture'])))
            item=scene.ssa.Instruction(62,(504,bound));bound+=1;count+=1
        if mode=='wrong_rotation' and op==169 and a[1] in (ids['rotated_pixel_x'],ids['rotated_pixel_y']):
            item=scene.ssa.Instruction(83,(8,a[1],a[4]));count+=1
        out.append(item)
    demand(count==({'shared_weight':3,'wrong_rotation':2}.get(mode,1)),'exact mutation '+mode+': '+str(count));h[3]=bound
    return scene.pack_module(h,out)


def main(argv=None):
    global WIDTH,HEIGHT
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--original',type=Path,required=True);p.add_argument('--record',type=Path,required=True)
    p.add_argument('--sanitizer',action='store_true');p.add_argument('--driver',action='append',choices=['intel','software'])
    p.add_argument('--dual-full',action='store_true',help='New dual-v2/v1 native path only; old full suite remains separate evidence')
    p.add_argument('--mode',action='append',choices=['enabled','off','coordinates','multiply_E_by_A','wrong_bottom_tile','wrong_R1_chart','shared_weight','wrong_rotation','full_multiply_E_by_A'])
    args=p.parse_args(argv);report={'schema':'native-scene-presenter-gpu-v1','status':'FAILED','steps':[],'devices':[],'native_activation':False,'video_color_chain_proven':False}
    code=1;private=None
    try:
        demand(not args.record.exists(),'exclusive evidence')
        original=args.original.read_bytes();profile=scene.load_profile()
        if args.dual_full:WIDTH=HEIGHT=3168
        candidate,record=(scene.transform_dual(original) if args.dual_full else scene.transform(original,profile))
        off,_=scene.transform(original,profile,False);demand(off==original,'OFF exact original bytes')
        owned=cases(profile)
        if args.dual_full:
            legacy=[c for c in owned if c['name'] in ['ON','OFF_same_command','ON_same_command_recovery','coverage_pattern_0','coverage_pattern_1','guard_0_3','guard_11_-3']]
            full=cases(FULL_PROFILE)
            # Switch formats/ordinary source with one immutable existing CB.
            owned=full+legacy+[full[0],full[1],legacy[0],full[2]]
        report.update(transformation=record,cases=owned)
        modes={'enabled':candidate,'off':off}
        if args.dual_full:
            newrecord={**record,'new_ssa_ids':record['v2_ssa_ids']}
            modes['wrong_rotation']=mutated(candidate,newrecord,'wrong_rotation')
            modes['full_multiply_E_by_A']=mutated(candidate,newrecord,'multiply_E_by_A')
        else:
            for mode in ['coordinates','multiply_E_by_A','wrong_bottom_tile','wrong_R1_chart','shared_weight']:modes[mode]=mutated(candidate,record,mode)
        selected=args.mode or list(modes)
        demand(all(mode in modes for mode in selected),'selected mode belongs to explicit profile scope')
        with tempfile.TemporaryDirectory(prefix='native-scene-gpu-') as directory:
            private=Path(directory);borrowed=(ROOT/'tests/native-fragment-guarded-presenter-gpu/fixture.cpp').read_text()
            borrowed=borrowed.replace('int main(int argc,char** argv){','int inherited_guard_main(int argc,char** argv){')
            borrowed=borrowed.replace('#include "../native-fragment-ssa-gpu/fixture.cpp"','#include "'+str(ROOT/'tests/native-fragment-ssa-gpu/fixture.cpp')+'"')
            borrowed=borrowed.replace('constexpr unsigned SourceWidth=688,SourceHeight=548;',f'constexpr unsigned SourceWidth={WIDTH},SourceHeight={HEIGHT};')
            (private/'borrowed_guard_fixture.h').write_text(borrowed)
            fixture=private/'fixture';vertex=private/'vertex.spv'
            compile_flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitizer else ['-O2']
            for cmd in [['g++','-std=c++17','-Wall','-Wextra','-Werror',*compile_flags,'-I',str(private),str(Path(__file__).with_name('fixture.cpp')),'-lvulkan','-o',str(fixture)],
                        ['glslc','--target-env=vulkan1.1',str(ROOT/'tests/native-fragment-ssa-gpu/fixture.vert'),'-o',str(vertex)]]:
                step=invoke(cmd);report['steps'].append(step);demand(step['returncode']==0,'fixture build')
            paths={}
            for mode in selected:
                path=private/(mode+'.spv');path.write_bytes(modes[mode]);paths[mode]=path
                val=invoke(['spirv-val','--target-env','vulkan1.1',str(path)]);report['steps'].append(val);demand(val['returncode']==0,'module validation '+mode)
            native_path=private/'original.spv';native_path.write_bytes(original)
            for driver,icd in [('intel','intel_icd.json'),('software','lvp_icd.json')]:
                if args.driver and driver not in args.driver:continue
                entry={'driver':driver,'runs':{}};report['devices'].append(entry)
                for mode in selected:
                    chosen=owned
                    if mode=='coordinates':chosen=[c for c in owned if c['mode']==9 or not c['valid']][:90]
                    if mode not in ['enabled','off','coordinates']:
                        if mode=='wrong_rotation':
                            chosen=owned[:3]+[next(c for c in owned if c['profile_revision']==2 and c.get('q',[0,0])[0]==F(.37) and .25<c['q'][1]<.5)]
                        else:
                            key={'multiply_E_by_A':'coverage_pattern_0','full_multiply_E_by_A':'coverage_pattern_0','wrong_bottom_tile':'global_seam_(0.37, 0.75)',
                                 'wrong_R1_chart':'native_routes_(0, 0, 1)','shared_weight':'native_routes_(1, 0, 0)'}[mode]
                            chosen=([next(c for c in owned if c['name']==key)] if mode=='full_multiply_E_by_A' else owned[:3]+[next(c for c in owned if c['name']==key)])
                    casefile=private/(mode+'.bin');casefile.write_bytes(pack(chosen,mode=='coordinates'))
                    settings=private/(driver+'-'+mode);settings.mkdir();log=settings/'validation.log'
                    (settings/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\nkhronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG\nkhronos_validation.report_flags = error,warn,perf\nkhronos_validation.log_filename = '+str(log)+'\n')
                    env=dict(os.environ,VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/'+icd,VK_LAYER_SETTINGS_PATH=str(settings),VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT')
                    result=invoke([str(fixture),str(vertex),str(native_path),str(paths[mode]),str(casefile),'coordinates' if mode=='coordinates' else 'off' if mode=='off' else 'enabled'],env,timeout=180)
                    result['validation_log']=log.read_text(errors='replace') if log.exists() else '';entry['runs'][mode]=result
                    demand('AddressSanitizer' not in result['stderr'] and 'runtime error:' not in result['stderr'],'sanitizer '+driver+'/'+mode)
                    demand('Validation Error' not in result['stderr']+result['validation_log'] and 'SYNC-HAZARD' not in result['validation_log'],'Vulkan/sync '+driver+'/'+mode)
                    if mode in ['enabled','off','coordinates']:
                        demand(result['returncode']==0,'actual GPU '+driver+'/'+mode);result['result']=json.loads(result['stdout'])
                    else:demand(result['returncode']!=0 and ('differs' in result['stderr'] and any(x in result['stderr'] for x in ['scene coverage alpha','scene contribution RGB'])),'valid mutant must fail numerical oracle '+mode)
            report['status'],code='PASS',0
    except Exception as error:report['error']=str(error)
    finally:
        sp=importlib.util.spec_from_file_location('scene_retention','/home/claw/gloomhaven_vr/scripts/town_run_retention.py');m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
        report['cleanup']=m.cleanup_completed_run('native-scene-presenter-gpu',code,args.record,ROOT,process_reaped=True)
        report['private_cache_removed']=private is None or not private.exists()
        # NaN appears only in deliberately invalid input cases, explicitly tagged.
        def finite_json(value):
            if isinstance(value,float) and not math.isfinite(value):return {'nonfinite_input':str(value)}
            if isinstance(value,dict):return {k:finite_json(v) for k,v in value.items()}
            if isinstance(value,list):return [finite_json(v) for v in value]
            return value
        args.record.write_text(json.dumps(finite_json(report),indent=2,allow_nan=False)+'\n')
    print(json.dumps({'status':report['status'],'error':report.get('error')}));return code


if __name__=='__main__':raise SystemExit(main())
