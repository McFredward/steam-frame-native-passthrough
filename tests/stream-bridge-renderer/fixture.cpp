// Execute the current production Renderer and Pool. Reuse the already audited
// public Vulkan fake API builder without rerunning its unrelated test main.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main inherited_pool_tests_not_run
#include "tests/stream-bridge-coverage-gpu/pool_tests.cpp"
#undef main
#define main inherited_clone_tests_not_run
#define checks clone_checks
#define require clone_require
#define info clone_info
#define object clone_object
#define Fixture CloneFixture
#include "tests/test_pixel_ssbo_clone_contract.cpp"
#undef Fixture
#undef object
#undef info
#undef require
#undef checks
#undef main
#pragma GCC diagnostic pop
#include "layer/stream_bridge_renderer.h"
#include "layer/stream_bridge_process.h"
#include <fstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace sr=stream_bridge_renderer;namespace sb=stream_bridge;namespace local=stream_bridge_local;
namespace {
std::vector<std::uint32_t> words(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);check(bool(f),"private source opens");const auto n=f.tellg();check(n>0&&n<65536&&n%4==0,"bounded private words");std::vector<std::uint32_t> out(std::size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),n);check(bool(f),"private source fully read");return out;}
sr::ProcessConsumer consumer{};std::vector<std::uint32_t> coverageWords,legacyWords;std::atomic<bool> leaseCurrent{true};
struct Driver:Fake {
 struct Update{std::uint64_t buffer;VkDeviceSize offset;float value;bool fill=false;};
 std::map<std::uint64_t,std::vector<Update>> commands;
 std::map<std::uint64_t,std::uint64_t> boundMemory,setBuffer,appControl;std::map<std::uint64_t,VkDeviceSize> bufferSizes,setOffset,appOffset;
 std::vector<float> reads;std::vector<VkFence> originalFences;std::vector<std::size_t> nativeSubmitCounts;std::vector<const void*> originalNexts,allForwardedExtensions;std::vector<VkFence> allSubmitFences;
 VkSemaphore wait=handle<VkSemaphore>(0x9871),signalSemaphore=handle<VkSemaphore>(0x9872);VkFence appFence=handle<VkFence>(0x9998);
 bool failOriginal=false;unsigned forwardOriginal=0,waitPreserved=0,signalPreserved=0,barrierReads=0,khrCalls=0;
 static Driver* self;Driver(){self=this;Fake::active=this;}
 static VkResult VKAPI_CALL beginCommand(VkCommandBuffer cb,const VkCommandBufferBeginInfo* i){++self->calls;check(i->flags==VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT||i->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,"owned guard immutable-SIMULT or upload one-time");self->commands[number(cb)].clear();return VK_SUCCESS;}
 static void VKAPI_CALL bufferUpdate(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,const void* bytes){check(offset%256==0&&size==4,"one immutable aligned guard float");float v;std::memcpy(&v,bytes,4);check(v==0||v==1,"guard only boolean");self->commands[number(cb)].push_back({number(buffer),offset,v,false});}
 static void VKAPI_CALL fillBuffer(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,std::uint32_t value){check(offset==0&&size==VK_WHOLE_SIZE&&value==0,"one immutable full-buffer zero fill");self->commands[number(cb)].push_back({number(buffer),0,0,true});}
 static VkResult VKAPI_CALL createBuffer(VkDevice,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* output){*output=self->make<VkBuffer>();self->bufferSizes[number(*output)]=info->size;return VK_SUCCESS;}
 static void VKAPI_CALL bufferReq(VkDevice,VkBuffer buffer,VkMemoryRequirements* output){*output={std::max<VkDeviceSize>(16384,self->bufferSizes.at(number(buffer))),1,1};}
 static VkResult VKAPI_CALL bind(VkDevice d,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize at){self->boundMemory[number(buffer)]=number(memory);return Fake::bindBuffer(d,buffer,memory,at);}
 static void VKAPI_CALL update(VkDevice d,std::uint32_t n,const VkWriteDescriptorSet* writes,std::uint32_t copies,const VkCopyDescriptorSet* c){
  if(n==1&&writes->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){check(writes->pBufferInfo&&writes->pBufferInfo->range==16,"control descriptor is sixteen-byte UBO");self->setBuffer[number(writes->dstSet)]=number(writes->pBufferInfo->buffer);self->setOffset[number(writes->dstSet)]=writes->pBufferInfo->offset;check(writes->pBufferInfo->offset%256==0,"each control descriptor has independent aligned byte offset");return;}
  Fake::update(d,n,writes,copies,c);
 }
 static void VKAPI_CALL barrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags dep,std::uint32_t nm,const VkMemoryBarrier* m,std::uint32_t nb,const VkBufferMemoryBarrier* b,std::uint32_t ni,const VkImageMemoryBarrier* image){
  if(nb){check(nb==1&&b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED,"guard queue ownership not guessed");check((src==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&dst==VK_PIPELINE_STAGE_TRANSFER_BIT&&b->srcAccessMask==VK_ACCESS_UNIFORM_READ_BIT&&b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT)||(src==VK_PIPELINE_STAGE_TRANSFER_BIT&&dst==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&b->dstAccessMask==VK_ACCESS_UNIFORM_READ_BIT),"guard has read-write and write-read GPU visibility");++self->barrierReads;return;}
  Fake::barrier(cb,src,dst,dep,nm,m,nb,b,ni,image);
 }
 static VkResult VKAPI_CALL createShader(VkDevice,const VkShaderModuleCreateInfo* ci,const VkAllocationCallbacks*,VkShaderModule* out){check(ci&&ci->codeSize&&ci->pCode,"production shader creation uses owned words");*out=self->make<VkShaderModule>();return VK_SUCCESS;}
 static void VKAPI_CALL destroyShader(VkDevice,VkShaderModule,const VkAllocationCallbacks*){}
 static VkResult VKAPI_CALL createPipelines(VkDevice,VkPipelineCache,std::uint32_t n,const VkGraphicsPipelineCreateInfo* ci,const VkAllocationCallbacks*,VkPipeline* out){check(n==1&&ci->stageCount==2,"production bounded original pipeline clone");*out=self->make<VkPipeline>();return VK_SUCCESS;}
 static void VKAPI_CALL destroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*){}
 static VkResult VKAPI_CALL createLayout(VkDevice,const VkPipelineLayoutCreateInfo* ci,const VkAllocationCallbacks*,VkPipelineLayout* out){check(ci->setLayoutCount==3,"native+mask+guard descriptor layouts");*out=self->make<VkPipelineLayout>();return VK_SUCCESS;}
 static void VKAPI_CALL destroyLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*){}
 static VkResult VKAPI_CALL freeSets(VkDevice,VkDescriptorPool,std::uint32_t,const VkDescriptorSet*){return VK_SUCCESS;}
 static VkResult VKAPI_CALL idle(VkDevice){return VK_SUCCESS;}
 static void execute(VkCommandBuffer cb){for(const auto& update:self->commands[number(cb)]){auto& bytes=self->memory.at(self->boundMemory.at(update.buffer));if(update.fill){check(self->bufferSizes.at(update.buffer)<=bytes.size(),"actual shared UBO fits queried allocation");std::memset(bytes.data(),0,std::size_t(self->bufferSizes.at(update.buffer)));}else{check(update.offset+4<=bytes.size(),"guard update inside allocation");std::memcpy(bytes.data()+update.offset,&update.value,4);}}auto app=self->appControl.find(number(cb));if(app!=self->appControl.end()){float v=0;auto& bytes=self->memory.at(self->boundMemory.at(app->second));std::memcpy(&v,bytes.data()+self->appOffset.at(number(cb)),4);self->reads.push_back(v);}}
 static VkResult VKAPI_CALL submit(VkQueue queue,std::uint32_t n,const VkSubmitInfo* ci,VkFence fence){auto& s=*self;++s.calls;++s.submits;check(queue==s.queue,"admitted original queue");s.allSubmitFences.push_back(fence);bool original=false;
  for(unsigned i=0;i<n;++i){if(ci[i].pNext)s.allForwardedExtensions.push_back(ci[i].pNext);if(ci[i].waitSemaphoreCount){check(ci[i].pWaitSemaphores&&*ci[i].pWaitSemaphores==s.wait&&ci[i].pWaitDstStageMask&&*ci[i].pWaitDstStageMask==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,"original wait semaphore/stage retained");++s.waitPreserved;}
   if(ci[i].signalSemaphoreCount){check(ci[i].pSignalSemaphores&&*ci[i].pSignalSemaphores==s.signalSemaphore,"original signal retained");++s.signalPreserved;}
   for(unsigned k=0;k<ci[i].commandBufferCount;++k){original|=s.appControl.count(number(ci[i].pCommandBuffers[k]));execute(ci[i].pCommandBuffers[k]);}}
  if(original){for(unsigned i=0;i<n;++i)for(unsigned k=0;k<ci[i].commandBufferCount;++k)if(s.appControl.count(number(ci[i].pCommandBuffers[k])))s.originalNexts.push_back(ci[i].pNext);++s.forwardOriginal;s.originalFences.push_back(fence);s.nativeSubmitCounts.push_back(n);if(s.failOriginal)return VK_ERROR_DEVICE_LOST;}
  if(fence){if(s.autoSignal)s.fences[number(fence)]=true;else s.pending.push_back(number(fence));}return VK_SUCCESS;
 }
 static VkResult VKAPI_CALL submit2(VkQueue q,std::uint32_t n,const VkSubmitInfo2* ci,VkFence fence){for(unsigned i=0;i<n;++i)for(unsigned k=0;k<ci[i].commandBufferInfoCount;++k)execute(ci[i].pCommandBufferInfos[k].commandBuffer);if(fence){if(self->autoSignal)self->fences[number(fence)]=true;else self->pending.push_back(number(fence));}check(q==self->queue,"submit2 exact queue");return self->failOriginal?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
 static VkResult VKAPI_CALL submit2KHR(VkQueue q,std::uint32_t n,const VkSubmitInfo2* ci,VkFence fence){++self->khrCalls;return submit2(q,n,ci,fence);}
 static PFN_vkVoidFunction VKAPI_CALL get(VkDevice d,const char* n){
#define FN(Name,Body) if(std::strcmp(n,"vk" #Name)==0)return reinterpret_cast<PFN_vkVoidFunction>(&Body);
  FN(BeginCommandBuffer,beginCommand) FN(CreateBuffer,createBuffer) FN(GetBufferMemoryRequirements,bufferReq) FN(CmdFillBuffer,fillBuffer) FN(CmdUpdateBuffer,bufferUpdate) FN(CmdPipelineBarrier,barrier) FN(BindBufferMemory,bind) FN(UpdateDescriptorSets,update)
  FN(CreateShaderModule,createShader) FN(DestroyShaderModule,destroyShader) FN(CreateGraphicsPipelines,createPipelines) FN(DestroyPipeline,destroyPipeline) FN(CreatePipelineLayout,createLayout) FN(DestroyPipelineLayout,destroyLayout) FN(FreeDescriptorSets,freeSets) FN(DeviceWaitIdle,idle) FN(QueueSubmit,submit) FN(QueueSubmit2,submit2) FN(QueueSubmit2KHR,submit2KHR)
#undef FN
  return Fake::get(d,n);
 }
};Driver* Driver::self=nullptr;
}
namespace stream_bridge_renderer {
bool prepareProcessCold()noexcept{return true;}bool enabled()noexcept{return true;}ProcessKind processKind()noexcept{return ProcessKind::Compositor;}
stream_bridge_native::NativeImages* sourceImages()noexcept{return nullptr;}void sourceExportedFd(int)noexcept{}
bool registerConsumerCold(ProcessConsumer c)noexcept{consumer=c;return true;}void unregisterConsumerCold(void*)noexcept{consumer={};}
bool readCoverageFragmentCold(std::vector<std::uint32_t>& out)noexcept{out=coverageWords;return true;}bool readLegacyFragmentCold(std::vector<std::uint32_t>& out)noexcept{out=legacyWords;return true;}
}
namespace stream_bridge_local {
struct LeaseReader::Impl{};LeaseReader::LeaseReader():impl_(new Impl){}LeaseReader::~LeaseReader()=default;
bool LeaseReader::mapReadOnly(int)noexcept{return true;}bool LeaseReader::current(const LeaseReference& v)const noexcept{return leaseCurrent&&v.sequence==71;}void LeaseReader::reset()noexcept{}
}
namespace stream_bridge_native {
struct NativeImages::Impl{};NativeImages::NativeImages():impl_(new Impl){}NativeImages::NativeImages(ExportPreparation,void*):NativeImages(){}NativeImages::~NativeImages()=default;
bool NativeImages::created(DeviceHandle,ImageHandle,const ImageDescription&)noexcept{return false;}bool NativeImages::allocated(DeviceHandle,MemoryHandle,bool)noexcept{return false;}bool NativeImages::bound(DeviceHandle,ImageHandle,MemoryHandle,std::uint64_t)noexcept{return false;}void NativeImages::destroyed(DeviceHandle,ImageHandle)noexcept{}void NativeImages::freed(DeviceHandle,MemoryHandle)noexcept{}bool NativeImages::exported(DeviceHandle,MemoryHandle,int,std::uint32_t,std::uintptr_t)noexcept{return false;}
}
namespace {
struct Integration {
 Driver vk;sr::Renderer renderer;CloneFixture native;
 std::array<VkDescriptorSetLayoutBinding,29> observedBindings{};
 VkDevice device=handle<VkDevice>(1);VkCommandBuffer cb=handle<VkCommandBuffer>(0x71000);VkImage image=handle<VkImage>(0x72000);VkImageView view=handle<VkImageView>(0x73000);VkDescriptorSet set=handle<VkDescriptorSet>(0x74000);VkDeviceMemory imported=handle<VkDeviceMemory>(0x75000);
 std::shared_ptr<std::atomic<bool>> connection=std::make_shared<std::atomic<bool>>(true);int file=-1;sr::LocalPicture picture;VkBuffer guard=VK_NULL_HANDLE;
 Integration(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps,unsigned imageCount=3,bool useColdProof=true,bool expectReady=true){
  leaseCurrent=true;float priority=1;VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queueInfo.queueFamilyIndex=0;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;native.device.queueCreateInfoCount=1;native.device.pQueueCreateInfos=&queueInfo;
  VkPhysicalDeviceMemoryProperties m{};m.memoryTypeCount=1;m.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  check(renderer.prepareCold(device,m,Driver::get,native.device,VK_API_VERSION_1_1,0),"actual renderer cold prepare registers process consumer");check(!consumer.owner,"pool preparation alone cannot advertise actual selected-image renderer");
  if(imageCount==1){
   // The retained native archive has ALL-stage scalar bindings 0..28, not
   // the old convenient seven-binding array3 fixture.
   for(unsigned n=0;n<observedBindings.size();++n)observedBindings[n]={n,n<8?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:n==8?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:n<25?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_ALL,nullptr};
   native.set0.bindingCount=observedBindings.size();native.set0.pBindings=observedBindings.data();
  }
  auto& definitions=renderer.definitions();check(definitions.createSet(native.nativeSet,native.set0,VK_SUCCESS),"actual native set definitions");check(definitions.createLayout(native.graphics.layout,native.layout,VK_SUCCESS),"actual native layout definitions");check(definitions.createRenderPass(native.graphics.renderPass,native.pass,VK_SUCCESS),"actual native renderpass definitions");
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=vs.size()*4;shader.pCode=vs.data();renderer.shaderCreated(native.stages[1].module,shader);shader.codeSize=ps.size()*4;shader.pCode=ps.data();
  if(useColdProof)renderer.shaderCreated(native.stages[0].module,shader);
  else check(definitions.createShader(native.stages[0].module,shader,VK_SUCCESS),"unpromoted exact native FS recorded without interface authority");
  native.colorBlend.blendEnable=VK_FALSE;
  renderer.graphicsCreated(VK_NULL_HANDLE,native.graphics,nullptr,handle<VkPipeline>(0x60));
  if(!expectReady){check(!consumer.owner,"pending or unpinned native count1 stays unadmitted");return;}
  check(consumer.owner&&consumer.coverage&&consumer.picture&&consumer.tick,"only both actual native companion pipelines register receiver route");
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.pNext=&external;info.imageType=VK_IMAGE_TYPE_2D;info.extent={16,2,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.format=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;renderer.imageCreated(image,info);
  char path[]="/tmp/sfnp-renderer-fixture-XXXXXX";file=mkstemp(path);check(file>=0,"owned kernel-object fixture fd");unlink(path);struct stat st{};check(!fstat(file,&st),"exact kernel object observed");VkImportMemoryFdInfoKHR fd{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};fd.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;fd.fd=file;VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.pNext=&fd;allocate.allocationSize=32;auto preparation=renderer.memoryPreparing(allocate);check(preparation.duplicate>=0,"fd duplicated BEFORE original consumes it");renderer.memoryAllocated(imported,allocate,preparation,VK_SUCCESS);renderer.imageBound(image,imported,0);
  VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};renderer.viewCreated(view,ci);writeView(view);
  picture.metadata.kind=local::Kind::Decoded;picture.metadata.descriptor={7,9,16,2};picture.metadata.binding[0]=31;picture.metadata.lease.kernel={std::uint64_t(st.st_dev),std::uint64_t(st.st_ino)};picture.metadata.lease.sequence=71;picture.metadata.peerConfirmed=true;picture.leases=std::make_shared<local::LeaseReader>();picture.connection=connection;
  sb::CertifiedAccessUnit certificate;certificate.descriptor=picture.metadata.descriptor;certificate.binding=picture.metadata.binding;certificate.coverageEncoding=sb::CoverageEncoding::Raw;certificate.coverage.assign(32,127);consumer.coverage(consumer.owner,certificate);consumer.picture(consumer.owner,picture);
  // Uploads become acquirable only after the ORIGINAL admitted queue drains
  // them and its real (fake-public-API) upload fence has signalled.
  renderer.submit(vk.queue,0,nullptr,VK_NULL_HANDLE);vk.signal();consumer.tick(consumer.owner);
  renderer.commandAllocated(handle<VkCommandBuffer>(0x70fff),VK_COMMAND_BUFFER_LEVEL_PRIMARY,0);renderer.commandAllocated(cb,VK_COMMAND_BUFFER_LEVEL_PRIMARY,0);
 }
 ~Integration(){renderer.stopCold();if(file>=0)close(file);}
 void writeView(VkImageView selected){VkDescriptorImageInfo imageInfo{};imageInfo.imageView=selected;imageInfo.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=9;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.descriptorCount=1;write.pImageInfo=&imageInfo;renderer.descriptorWrites(1,&write,0,nullptr);}
 sr::Draw record(VkCommandBufferUsageFlags flags=0){VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=flags;renderer.commandBegin(cb,&begin);renderer.pipelineBound(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,handle<VkPipeline>(0x60));renderer.setsBound(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,0,1,&set,0);auto result=renderer.beforeDraw(cb);if(result.control){guard=handle<VkBuffer>(vk.setBuffer.at(number(result.control)));vk.appControl[number(cb)]=number(guard);vk.appOffset[number(cb)]=vk.setOffset.at(number(result.control));}return result;}
 VkResult submit(bool semaphores=true){VkPipelineStageFlags stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&cb;if(semaphores){info.waitSemaphoreCount=info.signalSemaphoreCount=1;info.pWaitSemaphores=&vk.wait;info.pWaitDstStageMask=&stage;info.pSignalSemaphores=&vk.signalSemaphore;}return renderer.submit(vk.queue,1,&info,vk.appFence);}
};
void integration(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 {Integration f(vs,ps,1);auto draw=f.record();check(draw.ready&&draw.coverage&&draw.control,"documented native 29-binding count1 gets both companion pipelines");check(f.submit()==VK_SUCCESS&&f.vk.reads.back()==1,"native count1 current exact mask reaches original draw");}
 {Integration f(vs,ps,1,false,false);check(!f.record().ready,"pending count1 cannot be admitted by observed layout alone");}
 {auto wrong=ps;wrong.back()^=1;Integration f(vs,wrong,1,true,false);check(!f.record().ready,"mutated native fragment cannot authorize count1");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.ready&&draw.coverage&&draw.control&&draw.original==handle<VkPipeline>(0x60),"actual source import/descriptor/current uploaded mask selects coverage route");check(f.submit()==VK_SUCCESS&&f.vk.reads.back()==1,"zero then exact one guard reaches original native CB");check(f.vk.originalFences.back()==f.vk.appFence&&f.vk.waitPreserved==1&&f.vk.signalPreserved==1,"original fence and semaphore contract forwarded");
  check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"legal flags0 re-submit gets fresh immutable GPU guard without host UBO writes");leaseCurrent=false;check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"previously successful reused recording turns opaque once actual selected lease expires");
 }
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record coverage before connection loss");f.connection->store(false);check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"source connection loss revokes recorded draw before submit");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record exact descriptor before legal rewrite");f.writeView(handle<VkImageView>(0xdead));check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"descriptor rewrite after record cannot authorize a different native selected image");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record before unknown command");f.renderer.commandUnknown(f.cb);f.submit(false);check(f.vk.reads.back()==0,"unknown recorded command revokes guard");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record before reset");f.renderer.commandReset(f.cb);f.submit(false);check(f.vk.reads.back()==0,"reset drops old recorded picture and guard");}
 {Integration f(vs,ps);auto draw=f.record(VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);check(draw.ready&&!draw.control&&!draw.coverage,"SIMULT recording stays legacy/original even with ready mask");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"descriptor initially current");f.renderer.commandReset(f.cb);f.renderer.viewDestroyed(f.view);VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=f.image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};f.renderer.viewCreated(f.view,ci);draw=f.record();check(draw.ready&&!draw.control,"numerically reused view cannot revive an old descriptor serial");f.writeView(f.view);draw=f.record();check(draw.control!=VK_NULL_HANDLE,"actual new descriptor write restores current view");}
 {Integration f(vs,ps);f.record();f.vk.failOriginal=true;check(f.submit(false)==VK_ERROR_DEVICE_LOST,"original submit error returned unchanged");}
 {Integration f(vs,ps);f.record();VkProtectedSubmitInfo extension{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};extension.protectedSubmit=VK_FALSE;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&extension;info.commandBufferCount=1;info.pCommandBuffers=&f.cb;f.renderer.submit(f.vk.queue,1,&info,f.vk.appFence);check(f.vk.reads.back()==0&&f.vk.originalNexts.back()==&extension&&f.vk.originalFences.back()==f.vk.appFence,"unsupported submit pNext zeroes guard and forwards exact original extension/fence");}
 {Integration f(vs,ps);f.record();VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.cb;VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;check(f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence)==VK_SUCCESS&&f.vk.reads.back()==1,"actual submit2 path inserts exact current guard");const auto beforeKhr=f.vk.khrCalls;check(f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence,true)==VK_SUCCESS&&f.vk.reads.back()==1&&f.vk.khrCalls==beforeKhr+1,"KHR submit uses its actual distinct public dispatch");leaseCurrent=false;check(f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence)==VK_SUCCESS&&f.vk.reads.back()==0,"reused submit2 loses alpha when its actual lease expires");}
 {Integration f(vs,ps);f.record();std::array<VkSubmitInfo,129> infos{};std::array<VkProtectedSubmitInfo,129> extensions{};
  for(unsigned n=0;n<infos.size();++n){extensions[n].sType=VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;extensions[n].protectedSubmit=VK_FALSE;infos[n].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;infos[n].pNext=&extensions[n];}infos.back().commandBufferCount=1;infos.back().pCommandBuffers=&f.cb;
  f.vk.allSubmitFences.clear();f.vk.allForwardedExtensions.clear();check(f.renderer.submit(f.vk.queue,infos.size(),infos.data(),f.vk.appFence)==VK_SUCCESS,"129 native submit infos never become an invented error");check(f.vk.allForwardedExtensions.size()==129,"all native submit infos forwarded exactly once");for(unsigned n=0;n<extensions.size();++n)check(f.vk.allForwardedExtensions[n]==&extensions[n],"large-cardinality original pNext order preserved");check(f.vk.allSubmitFences.size()==3&&f.vk.allSubmitFences[0]==VK_NULL_HANDLE&&f.vk.allSubmitFences[1]==VK_NULL_HANDLE&&f.vk.allSubmitFences[2]==f.vk.appFence,"native fence appears only on final bounded chunk");check(f.vk.reads.back()==0,"large-cardinality fallback clears any previous guard before actual native draw");}
 {Integration f(vs,ps);f.record();f.renderer.trackingLost();f.submit(false);check(f.vk.reads.back()==0,"lost observer state revokes all old recordings");}
 {Integration f(vs,ps);f.record();for(unsigned n=0;n<stream_bridge_coverage_gpu::MaximumSubmissionFences;++n)check(f.submit(false)==VK_SUCCESS,"bounded unsignaled retirement reservation");f.submit(false);check(f.vk.reads.back()==0,"retirement capacity refusal must disable image guard before original submit");}
}
}
int main(int argc,char** argv){try{check(argc==5,"explicit private inputs required");auto vs=words(argv[1]),ps=words(argv[2]);coverageWords=words(argv[3]);legacyWords=words(argv[4]);integration(vs,ps);std::cout<<"{\"checks\":"<<checks<<",\"result\":\"PASS\"}\n";return 0;}catch(const std::exception& e){std::cerr<<"renderer failed after "<<checks<<": "<<e.what()<<'\n';return 1;}}
