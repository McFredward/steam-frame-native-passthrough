#!/usr/bin/env python3
"""Focused host proof. No device installation, Steam restart or network access."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'probe'))
import dashboard_transition_probe_spirv as transform
import dashboard_transition_probe_session as session


def require(value,message):
    if not value:raise ValueError(message)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--shipping',type=Path,required=True)
    p.add_argument('--library',type=Path,required=True)
    p.add_argument('--record',type=Path,required=True)
    p.add_argument('--suite',choices=('all','gpu','core','lifecycle'),default='all')
    a=p.parse_args()
    require(not a.record.exists(),'exclusive evidence required')
    report={'schema':'sfnp-dashboard-transition-local-proof-v1','status':'FAILED','steps':[],
        'hardware_actions':False,'installation_authorized':False,'shipping_bank_unchanged':True,'suite':a.suite,
        'negative_controls':[]}
    code=1
    def run(args,env=None,expected=0):
        result=subprocess.run([str(s) for s in args],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,timeout=120)
        item={'command':[str(s) for s in args],'returncode':result.returncode,'stdout':result.stdout.decode(errors='replace')[-5000:],
              'stderr':result.stderr.decode(errors='replace')[-5000:]}
        report['steps'].append(item)
        require((result.returncode==0) if expected==0 else result.returncode!=0,'command outcome: '+item['stderr'])
        return item
    try:
        source=a.shipping.read_bytes();instrumented,manifest=transform.instrument(source)
        report['transformation']=manifest
        _,before=transform.parse(source);_,after=transform.parse(instrumented)
        index=0
        for item in after:
            if index<len(before) and item==before[index]:index+=1
        require(index==len(before),'all original instructions retained as exact ordered subsequence')
        original_ids={transform.scene.result_id(i) for i in before if transform.scene.result_id(i) is not None}
        added=[i for i in after if i not in before]
        require(not any(transform.scene.result_id(i) in original_ids for i in added if transform.scene.result_id(i) is not None),
                'no original SSA definition replaced')
        report['original_instruction_subsequence']=len(before)
        altered=bytearray(source);altered[-1]^=1
        try:transform.instrument(altered)
        except Exception:
            report['wrong_source_pin_rejected']=True
            report['negative_controls'].append('wrong_shipping_pin')
        else:raise ValueError('wrong source admitted')
        with tempfile.TemporaryDirectory(prefix='dashboard-transition-focused-') as temporary:
            private=Path(temporary)
            artifact=private/'diagnostic.spv';artifact.write_bytes(instrumented)
            run(['spirv-val','--target-env','vulkan1.1',artifact])
            if a.suite in ('all','core'):
                core=private/'core'
                common=['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread']
                run(common+[ROOT/'probe/dashboard_transition_probe_tests.cpp',ROOT/'probe/dashboard_transition_probe.cpp','-o',core])
                run([core])
                for name,old,new in [
                    ('initial_terminal','if (requested != queuedPhase_.load(std::memory_order_acquire))',
                     'if (requested != queuedPhase_.load(std::memory_order_acquire) || (requested==3 && !phaseSpent_[3]))'),
                    ('missing_restore','CmdBindPipeline_(value, VK_PIPELINE_BIND_POINT_GRAPHICS, c->bound); c->swapped = false;',
                     'c->swapped = false;')]:
                    original=(ROOT/'probe/dashboard_transition_probe.cpp').read_text()
                    require(original.count(old)==1,'one exact lifecycle mutation anchor')
                    bad=private/(name+'.cpp');bad.write_text(original.replace(old,new).replace('#include "dashboard_transition_probe.h"','#include "'+str(ROOT/'probe/dashboard_transition_probe.h')+'"').replace('#include "../layer/pixel_snapshot_io.h"','#include "'+str(ROOT/'layer/pixel_snapshot_io.h')+'"'))
                    binary=private/name
                    run(common+[ROOT/'probe/dashboard_transition_probe_tests.cpp',bad,'-o',binary]);run([binary],expected=1)
                    report['negative_controls'].append(name)
                prepared=private/'prepared'
                receipt=session.prepare(prepared,a.library,a.shipping,host_fixture=True)
                require(receipt['installed'] is False and receipt['restart_performed'] is False,'helper stays preparation-only')
                require(session.status(prepared)['latest'] is None,'prepared session makes no false live status')
                try:session.collect(prepared)
                except ValueError:
                    report['unfenced_collection_rejected']=True
                    report['negative_controls'].append('unfenced_collection')
                else:raise ValueError('unfenced collection admitted')
                try:session.phase(prepared,1)
                except ValueError:
                    report['out_of_order_manual_phase_rejected']=True
                    report['negative_controls'].append('out_of_order_manual_phase')
                else:raise ValueError('manual stage skip admitted')
                session.phase(prepared,0)
                try:session.phase(prepared,0)
                except FileExistsError:
                    report['old_phase_rearm_rejected']=True
                    report['negative_controls'].append('old_phase_rearm')
                else:raise ValueError('old stage rearmed')
                report['prepared_session_machine']=receipt['machine']
            if a.suite!='core':
                borrowed=(ROOT/'tests/native-fragment-guarded-presenter-gpu/fixture.cpp').read_text()
                require(borrowed.count('int main(int argc,char** argv){')==1,'one borrowed fixture main')
                borrowed=borrowed.replace('int main(int argc,char** argv){','int inherited_guard_main(int argc,char** argv){')
                borrowed=borrowed.replace('#include "../native-fragment-ssa-gpu/fixture.cpp"','#include "'+str(ROOT/'tests/native-fragment-ssa-gpu/fixture.cpp')+'"')
                borrowed=borrowed.replace('constexpr unsigned SourceWidth=688,SourceHeight=548;','constexpr unsigned SourceWidth=512,SourceHeight=512;')
                (private/'borrowed_dashboard_guard_fixture.h').write_text(borrowed)
                gpu=private/'gpu';vertex=private/'vertex.spv'
                run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread','-I',private,ROOT/'probe/dashboard_transition_probe_gpu.cpp',ROOT/'probe/dashboard_transition_probe.cpp','-lvulkan','-o',gpu])
                run(['glslc','--target-env=vulkan1.1',ROOT/'tests/native-fragment-ssa-gpu/fixture.vert','-o',vertex])
                env=dict(os.environ,VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.json',XDG_CACHE_HOME=str(private/'driver-cache'),
                         VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT')
                gpu_prepared=private/'gpu-prepared';session.prepare(gpu_prepared,a.library,a.shipping,host_fixture=True)
                run([gpu,vertex,a.shipping,artifact,gpu_prepared]+(['lifecycle'] if a.suite=='lifecycle' else []),env)
                if a.suite!='lifecycle':
                    # Recreate the actual ambiguity in the first headset run:
                    # all otherwise valid companions compete for route0's slot.
                    # Only the diagnostic slot arithmetic changes; original
                    # image reads, guard, native color and tag gates stay intact.
                    _,route_instructions=transform.parse(instrumented)
                    route_words=list(struct.unpack('<%uI'%(len(instrumented)//4),instrumented))
                    ids=set(manifest['companion_slot_index_ssa']);zero=manifest['uint_zero_ssa'];cursor=5;changed=0
                    for instruction in route_instructions:
                        if instruction.opcode==128 and len(instruction.operands)==4 and instruction.operands[3] in ids:
                            # Remove only the route's contribution to its slot,
                            # retaining the original bounded tag predicate.
                            route_words[cursor+4]=zero
                            changed+=1
                        cursor+=1+len(instruction.operands)
                    require(changed==len(ids) and changed>0,'all first-winner arithmetic sites changed')
                    route_mutant=private/'shared-first-winner-mutant.spv'
                    route_mutant.write_bytes(struct.pack('<%uI'%len(route_words),*route_words))
                    run(['spirv-val','--target-env','vulkan1.1',route_mutant])
                    failed=run([gpu,vertex,a.shipping,route_mutant,gpu_prepared],env,expected=1)
                    require('second route has independent slot despite same phase and selector' in failed['stderr'],
                            'shared first-winner control fails on the actual simultaneous-route assertion')
                    report['negative_controls'].append('shared_first_winner_gpu')
                    _,instructions=transform.parse(instrumented);words=list(struct.unpack('<%uI'%(len(instrumented)//4),instrumented));cursor=5;changed=0
                    for instruction in instructions:
                        if instruction.opcode==62 and instruction.operands[0]==504 and instruction.operands[1]!=994:
                            words[cursor+2]=994;changed+=1
                        cursor+=1+len(instruction.operands)
                    require(changed==2,'both decoded output stores mutate to original opaque fallback')
                    mutant=private/'color-output-mutant.spv';mutant.write_bytes(struct.pack('<%uI'%len(words),*words))
                    run(['spirv-val','--target-env','vulkan1.1',mutant]);run([gpu,vertex,a.shipping,mutant,gpu_prepared],env,expected=1)
                    report['negative_controls'].append('changed_color_raster')
        report['private_cache_removed']=not private.exists()
        report['status']='PASS';code=0
    except Exception as error:
        report['error']=str(error)
    finally:
        cleanup_path=Path('/home/claw/gloomhaven_vr/scripts/town_run_retention.py')
        spec=importlib.util.spec_from_file_location('dashboard_retention',cleanup_path)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        report['cleanup']=module.cleanup_completed_run('dashboard-transition-probe',code,a.record,ROOT,process_reaped=True)
        a.record.parent.mkdir(parents=True,exist_ok=True)
        a.record.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'error':report.get('error'),'record':str(a.record)}))
    return code


if __name__=='__main__':raise SystemExit(main())
