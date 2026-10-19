// Appended to the existing Renderer fixture by run.py. Its public Vulkan fake
// executes the real production Renderer/Pool; no admission decision is copied.
int main(int argc,char** argv){try{
 check(argc==4,"explicit private inputs required");auto vs=words(argv[1]),ps=words(argv[2]);coverageWords=words(argv[3]);
 unsigned active=0,unmasked=0;
 for(unsigned delayed=0;delayed<2;++delayed){Integration f(vs,ps,1,true,true,false,false,false);
  const auto publish=[&](std::uint8_t value){sb::CertifiedAccessUnit c;c.descriptor=f.picture.metadata.descriptor;c.binding=f.picture.metadata.binding;c.coverageEncoding=sb::CoverageEncoding::Raw;c.coverage.assign(32,value);consumer.coverage(consumer.owner,c);};
  // Isolate local-message timing from the already known first-use-layout issue.
  // These finite resources are explicitly prewarmed, not assumed ready by a fake.
  for(unsigned n=0;n<stream_bridge_coverage_gpu::MaximumSlots;++n){++f.picture.metadata.descriptor.sourceFrame;publish(static_cast<std::uint8_t>(n+1));f.renderer.submit(f.vk.queue,0,nullptr,VK_NULL_HANDLE);f.vk.signal();consumer.tick(consumer.owner);}
  for(unsigned frame=0;frame<16;++frame){
   ++active;++f.picture.metadata.descriptor.sourceFrame;f.picture.metadata.lease.sequence=++leaseSequence;publish(static_cast<std::uint8_t>(frame+41));
   // Certificate/R8 are already available before CAPTURE in BOTH cases. Only
   // the exact decoded-picture message is delayed by one local worker phase.
   if(!delayed)consumer.picture(consumer.owner,f.picture);
   f.vk.appControl.erase(number(f.cb));f.vk.appMasks.erase(number(f.cb));
   const auto left=f.record(),right=f.renderer.beforeDraw(f.cb);
   if(delayed)consumer.picture(consumer.owner,f.picture);
   const auto guards=f.renderer.statsCold().guardAccepted;
   check(f.submit(false)==VK_SUCCESS,"native frame submitted unchanged after exact picture publication");
   const bool masked=left.ready&&right.ready&&f.renderer.statsCold().guardAccepted==guards+1;
   if(!masked)++unmasked;
   // Late arrival cannot retrofit the already recorded vkCmdBindPipeline.
   // Prove all actual resources ARE ready, and a real re-record can recover.
   check(f.record().ready,"same exact frame becomes eligible after actual native re-record");
   check(f.submit(false)==VK_SUCCESS,"re-recorded exact picture submits normally");
   f.vk.signal();consumer.tick(consumer.owner);
  }
 }
 std::printf("availability: active=%u, unmasked=%u; certificate-before-CAPTURE control; decoded message delayed one phase; no sleeps or guessed masks\n",active,unmasked);
 check(unmasked==0,"every active native frame must have its exact mask at first presentation");
 std::printf("PASS: %u actual Renderer/Pool timing checks\n",checks);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"REQUIREMENT RED after %u checks: %s\n",checks,e.what());return 1;}}
