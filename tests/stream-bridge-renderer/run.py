#!/usr/bin/env python3
"""Focused actual Renderer/Pool public-call integration; private pins are inputs.

No Frame access or Vulkan installation. The API fake executes the production
recorded guard writes in original queue order, not a copy of renderer decisions.
The existing standalone native-fragment and R8 GPU proofs remain separate.
"""
import argparse, hashlib, json, pathlib, shutil, subprocess

def main():
 p=argparse.ArgumentParser();p.add_argument('--native-root',required=True,type=pathlib.Path);p.add_argument('--output',required=True,type=pathlib.Path);p.add_argument('--vs',required=True,type=pathlib.Path);p.add_argument('--ps',required=True,type=pathlib.Path);p.add_argument('--coverage',required=True,type=pathlib.Path);p.add_argument('--sanitize',action='store_true');p.add_argument('--thread-sanitize',action='store_true');p.add_argument('--mutants',action='store_true');p.add_argument('--artifact-fallback-only',action='store_true');p.add_argument('--fallback-recovery-only',action='store_true');p.add_argument('--telemetry-only',action='store_true');p.add_argument('--timeline-only',action='store_true');p.add_argument('--reservation-recovery-only',action='store_true');p.add_argument('--continuous-only',action='store_true');p.add_argument('--idle-startup-only',action='store_true');p.add_argument('--descriptor-isolation-only',action='store_true');p.add_argument('--resource-pressure-only',action='store_true');p.add_argument('--camera-epoch-only',action='store_true');p.add_argument('--camera-pending-only',action='store_true');p.add_argument('--pressure-case',type=int,choices=range(7));a=p.parse_args()
 if sum((a.artifact_fallback_only,a.fallback_recovery_only,a.telemetry_only,a.timeline_only,a.reservation_recovery_only,a.continuous_only,a.idle_startup_only,a.descriptor_isolation_only,a.resource_pressure_only,a.camera_epoch_only,a.camera_pending_only))>1:p.error('choose one focused subset')
 if a.pressure_case is not None and not a.resource_pressure_only:p.error('--pressure-case requires --resource-pressure-only')
 here=pathlib.Path(__file__).resolve().parent;root=a.native_root.resolve();out=a.output.resolve();cache=out/'cache';cache.mkdir(parents=True,exist_ok=True);source=out/'source';source.mkdir(exist_ok=True)
 fixtures=here/'fixture.cpp';shutil.copy2(fixtures,source/'fixture.cpp')
 files=['layer/stream_bridge_renderer.cpp','layer/stream_bridge_renderer.h','layer/stream_bridge_process.h','layer/stream_bridge_artifacts.h','layer/stream_bridge_coverage_gpu.cpp','layer/stream_bridge_coverage_gpu.h','layer/native_presenter_pipeline.h','layer/submit_timeline_contract.h','tests/stream-bridge-coverage-gpu/pool_tests.cpp']
 pins={}
 for relative in files:
  original=root/relative;target=source/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(original,target);pins[relative]=hashlib.sha256(original.read_bytes()).hexdigest()
 renderer=source/'layer/stream_bridge_renderer.cpp';pool=source/'layer/stream_bridge_coverage_gpu.cpp'
 shared=[root/f for f in ['bridge/stream_bridge_frame.cpp','bridge/stream_bridge_auth.cpp','bridge/stream_bridge_protocol.cpp','bridge/hevc_bridge_sei.cpp']]
 args=[str(a.vs.resolve()),str(a.ps.resolve()),str(a.coverage.resolve())]
 if a.artifact_fallback_only:args+=['artifact-fallback']
 if a.fallback_recovery_only:args+=['fallback-recovery']
 if a.telemetry_only:args+=['telemetry']
 if a.timeline_only:args+=['timeline']
 if a.reservation_recovery_only:args+=['reservation-recovery']
 if a.continuous_only:args+=['continuous']
 if a.idle_startup_only:args+=['idle-startup']
 if a.descriptor_isolation_only:args+=['descriptor-isolation']
 if a.camera_epoch_only:args+=['camera-epoch']
 if a.camera_pending_only:args+=['camera-pending']
 if a.resource_pressure_only:args+=['resource-pressure' if a.pressure_case is None else 'pressure-'+str(a.pressure_case)]
 input_pins={str(path.resolve()):hashlib.sha256(path.read_bytes()).hexdigest() for path in [a.vs,a.ps,a.coverage]}
 def run(name,production,sanitize=False,negative=False,thread_sanitize=False):
  binary=cache/name;command=['c++','-std=c++17','-O1','-g','-Wall','-Wextra','-Wpedantic','-Werror','-Wno-missing-field-initializers','-I'+str(source),'-I'+str(root),'-I'+str(root/'layer'),'-pthread']
  if sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  if thread_sanitize:command+=['-fsanitize=thread','-fno-omit-frame-pointer']
  command += [str(source/'fixture.cpp'),str(production),str(pool)]+list(map(str,shared))+['-o',str(binary)]
  with (out/(name+'-build.log')).open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
  with (out/(name+'.log')).open('w') as log:status=subprocess.run([str(binary)]+args,stdout=log,stderr=subprocess.STDOUT).returncode
  if negative:
   if status==0:raise AssertionError(name+' survived causal control')
   expected={'startup-always-adds-zero':'idle startup forwards original submit pointer/count/queue/fence and dispatch unchanged','loss-skips-retained-zero':'tracking loss retains mandatory zeroing for previously guarded Submit2 recordings','unrelated-pool-erases-video':'unrelated native UI pool reset cannot erase video descriptor or lose an active mask','copy-erases-video-globally':'copying video image into a UI descriptor never invalidates source video descriptor','allocation-aba-admitted':"reallocated identical numeric set/view cannot revive command's old allocation generation"}
   expected.update({'pre-camera-upload-blocked':'every authenticated pending mask drains before the first camera grant without requiring a masked recording','camera-epoch-draw-bypassed':'late camera acknowledgment cannot activate only the second eye of an existing command buffer','camera-epoch-collect-bypassed':'revoke and reacknowledge cannot revive any already-recorded stereo coverage at submit','camera-draw-bypassed':'missing camera acknowledgment refuses new masked draws','camera-collect-bypassed':'camera readiness revocation zeroes already-recorded masked command','image-budget-too-small':'native video image recovers after eighty unrelated imported image generations','memory-budget-too-small':'native video memory recovers after eighty unrelated imported allocations','view-budget-too-small':'native video view recovers after three hundred unrelated native views','untracked-write-reserves-entry':'untracked native UI writes do not consume video descriptor capacity','untracked-copy-reserves-entry':'untracked native UI copies do not consume video descriptor capacity'})
   if name in expected and expected[name] not in (out/(name+'.log')).read_text():raise AssertionError(name+' failed outside its intended lifecycle assertion')
  elif status:
   diagnostic=(out/(name+'.log')).read_text()
   if thread_sanitize and 'FATAL: ThreadSanitizer: unexpected memory mapping' in diagnostic:return {'exit_status':status,'unavailable':True,'reason':'ThreadSanitizer cannot initialize its shadow address space in this host; no race-free claim.'}
   raise AssertionError(name+' failed, see '+str(out/(name+'.log')))
  if a.telemetry_only and not negative:
   lines=(out/(name+'.log')).read_text()
   if lines.count('native pipeline refused: image interface not verified')!=1 or 'native count1 image interface verified' not in lines or 'native exact-coverage pipeline prepared; original picture preserved' not in lines:raise AssertionError('startup refusal spam consumed later admission diagnostics')
   if lines.count('renderer flow:')>48:raise AssertionError('cold renderer diagnostics exceeded bounded fixture status')
  result={'exit_status':status,'expected_negative':negative}
  if status==0:
   for line in (out/(name+'.log')).read_text().splitlines():
    if line.startswith('{'):
     try:result['checks']=json.loads(line)['checks']
     except (ValueError,KeyError):pass
  return result
 results={'normal':run('normal',renderer)}
 if a.sanitize:results['sanitize']=run('sanitize',renderer,True)
 if a.thread_sanitize:results['thread-sanitize']=run('thread-sanitize',renderer,thread_sanitize=True)
 if a.mutants:
  text=renderer.read_text()
  mutations={
   'pre-camera-upload-blocked':('p.uploadsPending.exchange(false,std::memory_order_acq_rel)','p.guardedRecordingPossible.load(std::memory_order_acquire)&&p.uploadsPending.exchange(false,std::memory_order_acq_rel)'),
   'camera-epoch-draw-bypassed':('if(!command->cameraEpoch||command->cameraEpoch!=cameraAlphaEpoch()){p.refused(DrawRefusal::Camera);return out;}','if(!cameraAlphaReady()){p.refused(DrawRefusal::Camera);return out;}'),
   'camera-epoch-collect-bypassed':('if(!command->cameraEpoch||command->cameraEpoch!=cameraAlphaEpoch())return refuse(CollectRefusal::Camera);','if(!cameraAlphaReady())return refuse(CollectRefusal::Camera);'),
   'camera-draw-bypassed':('if(!command->cameraEpoch||command->cameraEpoch!=cameraAlphaEpoch()){p.refused(DrawRefusal::Camera);return out;}','if(false){p.refused(DrawRefusal::Camera);return out;}'),
   'camera-collect-bypassed':('if(!command->cameraEpoch||command->cameraEpoch!=cameraAlphaEpoch())return refuse(CollectRefusal::Camera);','if(false)return refuse(CollectRefusal::Camera);'),
   'image-budget-too-small':('std::array<Image,512> images{};','std::array<Image,64> images{};'),
   'memory-budget-too-small':('std::array<Memory,512> memories{};','std::array<Memory,64> memories{};'),
   'view-budget-too-small':('std::array<View,1024> views{};','std::array<View,256> views{};'),
   'untracked-write-reserves-entry':('if(!view){if(entry)*entry={};if(supported)','if(!view){if(!entry)add(p.descriptors,handle(write.dstSet));if(entry)*entry={};if(supported)'),
   'untracked-copy-reserves-entry':('if(!view||view->serial!=prior.viewSerial){if(destination)*destination={};','if(!view||view->serial!=prior.viewSerial){if(!destination)add(p.descriptors,handle(update.dstSet));if(destination)*destination={};'),
   'stale-lease-admitted':('require(current(use.picture),CollectRefusal::Lease);','require(true,CollectRefusal::Lease);'),
   'missing-zero-on-reuse':('submits[0].pCommandBuffers=&p.zero;','submits[0].pCommandBuffers=&p.zero;submits[0].commandBufferCount=0;'),
   'missing-native-count1-proof':('p.definitions.authorizeProbeInterface(proof->profile)','false'),
   'native-handle-replaced':('entry->layout=coverageLayout;create(p.coverageWords,entry->layout,entry->coverage);','entry->layout=coverageLayout;create(p.coverageWords,entry->layout,entry->coverage);original=entry->coverage;'),
   'unsupported-submit-poisons-future':('if(!supported){','if(!supported){p.pool.prepareSubmit(queue,nullptr,gpu::MaximumSlots+1);'),
   'pending-misreported-ready':('out.ready=gpu.ready;','out.ready=gpu.ready+gpu.pending;'),
   'pre-reservation-poisons-pool':('auto&p=*impl_;if(!n)return {};', 'auto&p=*impl_;if(!n)return {};std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){for(std::size_t i=0;ls&&i<n&&i<MaximumSlots;++i)if(ls[i])p.slots[ls[i].slot].quarantine.store(true);return {};}'),
   'post-submit-mutex-poisons-pool':('if(!p.prepared||q!=p.queue||!ls||!n||n>MaximumSlots){refuse();return;}', 'std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){refuse();return;}if(!p.prepared||q!=p.queue||!ls||!n||n>MaximumSlots){refuse();return;}'),
   'acquire-takes-cold-mutex':('return impl_->acquire(f,true);','std::unique_lock<std::mutex> lock(impl_->gate,std::try_to_lock);if(!lock)return {};return impl_->acquire(f,true);'),
   'copy-takes-cold-mutex':('auto&p=*impl_;if(!p.prepared||q!=p.queue)return;', 'auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock||!p.prepared||q!=p.queue)return;'),
   'reserve-takes-cold-mutex':('auto&p=*impl_;if(!n)return {};', 'auto&p=*impl_;if(!n)return {};std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock)return {};'),
   'canonical-timeline-rejected':('if(timeline==stream_bridge_submit::TimelineStatus::Extension)why=SubmitRefusal::Extension;','if(info.pNext)why=SubmitRefusal::Extension;'),
   'none-misreported-mask':('++p.counts.maskNone;return;','++p.counts.maskDecoded;return;'),
   'startup-always-adds-zero':('||!p.guardedRecordingPossible.load(std::memory_order_acquire)', ''),
   'loss-skips-retained-zero':('||!p.guardedRecordingPossible.load(std::memory_order_acquire)', '||!p.guardedRecordingPossible.load(std::memory_order_acquire)||p.lost.load(std::memory_order_acquire)'),
   'unrelated-pool-erases-video':('allocation.id&&allocation.pool==handle(pool)', 'allocation.id&&((void)pool,true)'),
   'copy-erases-video-globally':('++p.counts.descriptorCopies;', '++p.counts.descriptorCopies;for(auto& entry:p.descriptors)entry.valid=false;'),
   'allocation-aba-admitted':('&&allocation->generation==use.descriptorGeneration', ''),
  }
  if a.artifact_fallback_only:mutations={}
  if a.camera_pending_only:mutations={name:value for name,value in mutations.items() if name in ('pre-camera-upload-blocked','startup-always-adds-zero','loss-skips-retained-zero','camera-epoch-draw-bypassed','camera-epoch-collect-bypassed')}
  if a.camera_epoch_only:mutations={name:value for name,value in mutations.items() if name in ('camera-epoch-draw-bypassed','camera-epoch-collect-bypassed')}
  if a.resource_pressure_only:mutations={name:value for name,value in mutations.items() if name in ('camera-draw-bypassed','camera-collect-bypassed','image-budget-too-small','memory-budget-too-small','view-budget-too-small','untracked-write-reserves-entry','untracked-copy-reserves-entry')}
  if a.reservation_recovery_only:mutations={name:value for name,value in mutations.items() if name in ('pre-reservation-poisons-pool','post-submit-mutex-poisons-pool')}
  if a.continuous_only:mutations={name:value for name,value in mutations.items() if name in ('acquire-takes-cold-mutex','copy-takes-cold-mutex','reserve-takes-cold-mutex')}
  if a.timeline_only:mutations={name:value for name,value in mutations.items() if name=='canonical-timeline-rejected'}
  if a.fallback_recovery_only:mutations={name:value for name,value in mutations.items() if name=='unsupported-submit-poisons-future'}
  if a.telemetry_only:mutations={name:value for name,value in mutations.items() if name in ('pending-misreported-ready','none-misreported-mask')}
  if a.idle_startup_only:mutations={name:value for name,value in mutations.items() if name in ('startup-always-adds-zero','loss-skips-retained-zero')}
  if a.descriptor_isolation_only:mutations={name:value for name,value in mutations.items() if name in ('unrelated-pool-erases-video','copy-erases-video-globally','allocation-aba-admitted')}
  for name,(old,new) in mutations.items():
   pool_mutant=name in ('pre-reservation-poisons-pool','post-submit-mutex-poisons-pool','acquire-takes-cold-mutex','copy-takes-cold-mutex','reserve-takes-cold-mutex');original_text=pool.read_text() if pool_mutant else text
   if old not in original_text:raise AssertionError('semantic control anchor missing: '+name)
   path=source/(name+'.cpp');path.write_text(original_text.replace(old,new) if name in ('pre-camera-upload-blocked','startup-always-adds-zero','loss-skips-retained-zero') else original_text.replace(old,new,1))
   if pool_mutant:
    original_pool=pool;pool=path
    try:results[name]=run(name,renderer,negative=True)
    finally:pool=original_pool
   elif name=='unsupported-submit-poisons-future':
    original_pool=pool;pool_text=pool.read_text();anchor='if(!p.prepared||!p.ownerGeneration||q!=p.queue||!p.leases(ls,n,mask,gs)||p.ticketSerial==(std::numeric_limits<std::uint64_t>::max()>>3)){++p.refusals;return {};}'
    if anchor not in pool_text:raise AssertionError('unsupported submit whole-pool causal anchor missing')
    pool=source/(name+'-pool.cpp');pool.write_text(pool_text.replace(anchor,anchor.replace('++p.refusals;', 'if(!ls||n>MaximumSlots)for(auto& s:p.slots)s.quarantine.store(true);++p.refusals;'),1))
    try:results[name]=run(name,path,negative=True)
    finally:pool=original_pool
   else:
    original_args=args
    pressure_modes={'camera-draw-bypassed':5,'camera-collect-bypassed':5,'image-budget-too-small':0,'memory-budget-too-small':1,'view-budget-too-small':2,'untracked-write-reserves-entry':3,'untracked-copy-reserves-entry':4}
    if name in pressure_modes:args=args[:3]+['pressure-'+str(pressure_modes[name])]
    if a.camera_pending_only and name in ('startup-always-adds-zero','loss-skips-retained-zero'):args=args[:3]+['idle-startup']
    try:results[name]=run(name,path,negative=True)
    finally:args=original_args
 (out/'results.json').write_text(json.dumps({'results':results,'sources':pins,'fixture_sha256':hashlib.sha256((source/'fixture.cpp').read_bytes()).hexdigest(),'private_inputs':input_pins,'scope':'Actual production Renderer and Pool with forwarded public-call fake, explicit external auth/lease observations; not a native Steam/Frame or GPU-picture test.'},indent=2)+'\n')
 print(json.dumps(results))

if __name__=='__main__':main()
