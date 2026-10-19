#!/usr/bin/env python3
"""Focused actual Renderer/Pool public-call integration; private pins are inputs.

No Frame access or Vulkan installation. The API fake executes the production
recorded guard writes in original queue order, not a copy of renderer decisions.
The existing standalone native-fragment and R8 GPU proofs remain separate.
"""
import argparse, hashlib, json, pathlib, shutil, subprocess

def main():
 p=argparse.ArgumentParser();p.add_argument('--native-root',required=True,type=pathlib.Path);p.add_argument('--output',required=True,type=pathlib.Path);p.add_argument('--vs',required=True,type=pathlib.Path);p.add_argument('--ps',required=True,type=pathlib.Path);p.add_argument('--coverage',required=True,type=pathlib.Path);p.add_argument('--sanitize',action='store_true');p.add_argument('--mutants',action='store_true');p.add_argument('--artifact-fallback-only',action='store_true');p.add_argument('--fallback-recovery-only',action='store_true');a=p.parse_args()
 if a.artifact_fallback_only and a.fallback_recovery_only:p.error('choose one focused subset')
 here=pathlib.Path(__file__).resolve().parent;root=a.native_root.resolve();out=a.output.resolve();cache=out/'cache';cache.mkdir(parents=True,exist_ok=True);source=out/'source';source.mkdir(exist_ok=True)
 fixtures=here/'fixture.cpp';shutil.copy2(fixtures,source/'fixture.cpp')
 files=['layer/stream_bridge_renderer.cpp','layer/stream_bridge_renderer.h','layer/stream_bridge_process.h','layer/stream_bridge_artifacts.h','layer/stream_bridge_coverage_gpu.cpp','layer/stream_bridge_coverage_gpu.h','layer/native_presenter_pipeline.h']
 pins={}
 for relative in files:
  original=root/relative;target=source/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(original,target);pins[relative]=hashlib.sha256(original.read_bytes()).hexdigest()
 renderer=source/'layer/stream_bridge_renderer.cpp';pool=source/'layer/stream_bridge_coverage_gpu.cpp'
 shared=[root/f for f in ['bridge/stream_bridge_frame.cpp','bridge/stream_bridge_auth.cpp','bridge/stream_bridge_protocol.cpp','bridge/hevc_bridge_sei.cpp']]
 args=[str(a.vs.resolve()),str(a.ps.resolve()),str(a.coverage.resolve())]
 if a.artifact_fallback_only:args+=['artifact-fallback']
 if a.fallback_recovery_only:args+=['fallback-recovery']
 input_pins={str(path.resolve()):hashlib.sha256(path.read_bytes()).hexdigest() for path in [a.vs,a.ps,a.coverage]}
 def run(name,production,sanitize=False,negative=False):
  binary=cache/name;command=['c++','-std=c++17','-O1','-g','-Wall','-Wextra','-Wpedantic','-Werror','-Wno-missing-field-initializers','-I'+str(source),'-I'+str(root),'-I'+str(root/'layer'),'-pthread']
  if sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  command += [str(source/'fixture.cpp'),str(production),str(pool)]+list(map(str,shared))+['-o',str(binary)]
  with (out/(name+'-build.log')).open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
  with (out/(name+'.log')).open('w') as log:status=subprocess.run([str(binary)]+args,stdout=log,stderr=subprocess.STDOUT).returncode
  if negative:
   if status==0:raise AssertionError(name+' survived causal control')
  elif status:raise AssertionError(name+' failed, see '+str(out/(name+'.log')))
  return {'exit_status':status,'expected_negative':negative}
 results={'normal':run('normal',renderer)}
 if a.sanitize:results['sanitize']=run('sanitize',renderer,True)
 if a.mutants:
  text=renderer.read_text()
  mutations={
   'stale-lease-admitted':('valid=valid&&current(use.picture);','valid=valid&&true;'),
   'missing-zero-on-reuse':('submits[0].pCommandBuffers=&p.zero;','submits[0].pCommandBuffers=&p.zero;submits[0].commandBufferCount=0;'),
   'missing-native-count1-proof':('p.definitions.authorizeProbeInterface(proof->profile)','false'),
   'native-handle-replaced':('entry->layout=coverageLayout;create(p.coverageWords,entry->layout,entry->coverage);','entry->layout=coverageLayout;create(p.coverageWords,entry->layout,entry->coverage);original=entry->coverage;'),
   'unsupported-submit-poisons-future':('if(!supported){','if(!supported){p.pool.prepareSubmit(queue,nullptr,gpu::MaximumSlots+1);'),
  }
  if a.artifact_fallback_only:mutations={}
  if a.fallback_recovery_only:mutations={name:value for name,value in mutations.items() if name=='unsupported-submit-poisons-future'}
  for name,(old,new) in mutations.items():
   if old not in text:raise AssertionError('semantic control anchor missing: '+name)
   path=source/(name+'.cpp');path.write_text(text.replace(old,new,1));results[name]=run(name,path,negative=True)
 (out/'results.json').write_text(json.dumps({'results':results,'sources':pins,'fixture_sha256':hashlib.sha256((source/'fixture.cpp').read_bytes()).hexdigest(),'private_inputs':input_pins,'scope':'Actual production Renderer and Pool with forwarded public-call fake, explicit external auth/lease observations; not a native Steam/Frame or GPU-picture test.'},indent=2)+'\n')
 print(json.dumps(results))

if __name__=='__main__':main()
