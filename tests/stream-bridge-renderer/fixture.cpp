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
#include <functional>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace sr=stream_bridge_renderer;namespace sb=stream_bridge;namespace local=stream_bridge_local;
namespace {
std::vector<std::uint32_t> words(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);check(bool(f),"private source opens");const auto n=f.tellg();check(n>0&&n<65536&&n%4==0,"bounded private words");std::vector<std::uint32_t> out(std::size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),n);check(bool(f),"private source fully read");return out;}
sr::ProcessConsumer consumer{};std::vector<std::uint32_t> coverageWords;std::atomic<bool> leaseCurrent{true};std::atomic<std::uint64_t> leaseSequence{71};bool coverageReadable=true;
struct Driver:Fake {
 struct Update{std::uint64_t buffer;VkDeviceSize offset;float value;bool fill=false;};
 std::map<std::uint64_t,std::vector<Update>> commands;
 std::map<std::uint64_t,std::uint64_t> boundMemory,setBuffer,appControl;std::map<std::uint64_t,VkDeviceSize> bufferSizes,setOffset,appOffset;
 struct Copy{VkBuffer buffer;VkImage image;VkDeviceSize offset;std::size_t bytes;};
 std::map<std::uint64_t,std::vector<Copy>> imageCopies;
 std::map<std::uint64_t,std::uint64_t> viewImage,coverageView;
 std::map<std::uint64_t,std::vector<std::uint8_t>> imageBytes;
 std::map<std::uint64_t,std::vector<VkDescriptorSet>> appMasks;
 std::vector<std::vector<std::uint8_t>> sampledMasks;std::vector<VkCommandBuffer> executedCommands;
 std::vector<float> reads;std::vector<VkFence> originalFences;std::vector<std::size_t> nativeSubmitCounts;std::vector<const void*> originalNexts,allForwardedExtensions;std::vector<VkFence> allSubmitFences;
 struct Forward{VkQueue queue;std::uint32_t count;const void* infos;VkFence fence;bool sync2;};
 std::vector<Forward> forwarded;std::map<std::uint64_t,unsigned> ordinaryMarks;std::vector<unsigned> ordinaryExecuted;
 std::vector<VkSubmitInfo> lastSubmitInfos;std::vector<VkCommandBuffer> lastSubmitCommands;
 std::vector<VkPipeline> createdPipelines,destroyedPipelines;
 VkSemaphore wait=handle<VkSemaphore>(0x9871),signalSemaphore=handle<VkSemaphore>(0x9872);VkFence appFence=handle<VkFence>(0x9998);
 bool failOriginal=false,failCoverage=false;unsigned forwardOriginal=0,waitPreserved=0,signalPreserved=0,barrierReads=0,khrCalls=0;
 std::atomic<bool> blockPipeline{false},insidePipeline{false};
 std::atomic<bool> blockFenceStatus{false},insideFenceStatus{false};
 std::function<void()> afterOriginal;
 static Driver* self;Driver(){self=this;Fake::active=this;}
 static VkResult VKAPI_CALL beginCommand(VkCommandBuffer cb,const VkCommandBufferBeginInfo* i){++self->calls;check(i->flags==VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT||i->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,"owned guard immutable-SIMULT or upload one-time");self->commands[number(cb)].clear();return VK_SUCCESS;}
 static void VKAPI_CALL bufferUpdate(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,const void* bytes){check(offset%256==0&&size==4,"one immutable aligned guard float");float v;std::memcpy(&v,bytes,4);check(v==0||v==1,"guard only boolean");self->commands[number(cb)].push_back({number(buffer),offset,v,false});}
 static void VKAPI_CALL fillBuffer(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,std::uint32_t value){check(offset==0&&size==VK_WHOLE_SIZE&&value==0,"one immutable full-buffer zero fill");self->commands[number(cb)].push_back({number(buffer),0,0,true});}
 static VkResult VKAPI_CALL createBuffer(VkDevice,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* output){*output=self->make<VkBuffer>();self->bufferSizes[number(*output)]=info->size;return VK_SUCCESS;}
 static void VKAPI_CALL bufferReq(VkDevice,VkBuffer buffer,VkMemoryRequirements* output){*output={std::max<VkDeviceSize>(16384,self->bufferSizes.at(number(buffer))),1,1};}
 static VkResult VKAPI_CALL bind(VkDevice d,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize at){self->boundMemory[number(buffer)]=number(memory);return Fake::bindBuffer(d,buffer,memory,at);}
 static void VKAPI_CALL update(VkDevice d,std::uint32_t n,const VkWriteDescriptorSet* writes,std::uint32_t copies,const VkCopyDescriptorSet* c){
  if(n==1&&writes->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){check(writes->pBufferInfo&&writes->pBufferInfo->range==16,"control descriptor is sixteen-byte UBO");self->setBuffer[number(writes->dstSet)]=number(writes->pBufferInfo->buffer);self->setOffset[number(writes->dstSet)]=writes->pBufferInfo->offset;check(writes->pBufferInfo->offset%256==0,"each control descriptor has independent aligned byte offset");return;}
  if(n==1&&writes->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER&&writes->pImageInfo)self->coverageView[number(writes->dstSet)]=number(writes->pImageInfo->imageView);
  Fake::update(d,n,writes,copies,c);
 }
 static VkResult VKAPI_CALL createView(VkDevice device,const VkImageViewCreateInfo* info,const VkAllocationCallbacks* allocator,VkImageView* view){const auto result=Fake::createImageView(device,info,allocator,view);if(result==VK_SUCCESS)self->viewImage[number(*view)]=number(info->image);return result;}
 static void VKAPI_CALL copyImage(VkCommandBuffer cb,VkBuffer buffer,VkImage image,VkImageLayout layout,std::uint32_t count,const VkBufferImageCopy* regions){Fake::copy(cb,buffer,image,layout,count,regions);check(!regions->bufferRowLength&&!regions->bufferImageHeight,"trace copy uses production tightly packed R8 rows");auto& copies=self->imageCopies[number(cb)];copies.clear();copies.push_back({buffer,image,regions->bufferOffset,std::size_t(regions->imageExtent.width)*regions->imageExtent.height});}
 static void VKAPI_CALL barrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags dep,std::uint32_t nm,const VkMemoryBarrier* m,std::uint32_t nb,const VkBufferMemoryBarrier* b,std::uint32_t ni,const VkImageMemoryBarrier* image){
  if(nb){check(nb==1&&b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED,"guard queue ownership not guessed");check((src==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&dst==VK_PIPELINE_STAGE_TRANSFER_BIT&&b->srcAccessMask==VK_ACCESS_UNIFORM_READ_BIT&&b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT)||(src==VK_PIPELINE_STAGE_TRANSFER_BIT&&dst==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&b->dstAccessMask==VK_ACCESS_UNIFORM_READ_BIT),"guard has read-write and write-read GPU visibility");++self->barrierReads;return;}
  Fake::barrier(cb,src,dst,dep,nm,m,nb,b,ni,image);
 }
 static VkResult VKAPI_CALL createShader(VkDevice,const VkShaderModuleCreateInfo* ci,const VkAllocationCallbacks*,VkShaderModule* out){check(ci&&ci->codeSize&&ci->pCode,"production shader creation uses owned words");*out=self->make<VkShaderModule>();return VK_SUCCESS;}
 static void VKAPI_CALL destroyShader(VkDevice,VkShaderModule,const VkAllocationCallbacks*){}
 static VkResult VKAPI_CALL createPipelines(VkDevice,VkPipelineCache,std::uint32_t n,const VkGraphicsPipelineCreateInfo* ci,const VkAllocationCallbacks*,VkPipeline* out){check(n==1&&ci->stageCount==2,"production bounded original pipeline clone");self->insidePipeline=true;while(self->blockPipeline.load())std::this_thread::yield();self->insidePipeline=false;if(self->failCoverage){*out={};return VK_ERROR_OUT_OF_DEVICE_MEMORY;}*out=self->make<VkPipeline>();self->createdPipelines.push_back(*out);return VK_SUCCESS;}
 static void VKAPI_CALL destroyPipeline(VkDevice,VkPipeline value,const VkAllocationCallbacks*){check(std::find(self->destroyedPipelines.begin(),self->destroyedPipelines.end(),value)==self->destroyedPipelines.end(),"pipeline destroyed only once");self->destroyedPipelines.push_back(value);}
 static VkResult VKAPI_CALL createLayout(VkDevice,const VkPipelineLayoutCreateInfo* ci,const VkAllocationCallbacks*,VkPipelineLayout* out){check(ci->setLayoutCount==3,"native+mask+guard descriptor layouts");*out=self->make<VkPipelineLayout>();return VK_SUCCESS;}
 static void VKAPI_CALL destroyLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*){}
 static VkResult VKAPI_CALL freeSets(VkDevice,VkDescriptorPool,std::uint32_t,const VkDescriptorSet*){return VK_SUCCESS;}
 static VkResult VKAPI_CALL idle(VkDevice){return VK_SUCCESS;}
 static VkResult VKAPI_CALL fenceStatus(VkDevice device,VkFence fence){self->insideFenceStatus=true;while(self->blockFenceStatus.load())std::this_thread::yield();self->insideFenceStatus=false;return Fake::fenceStatus(device,fence);}
 static void execute(VkCommandBuffer cb){self->executedCommands.push_back(cb);if(self->ordinaryMarks.count(number(cb)))self->ordinaryExecuted.push_back(self->ordinaryMarks.at(number(cb)));for(const auto& copy:self->imageCopies[number(cb)]){const auto& bytes=self->memory.at(self->boundMemory.at(number(copy.buffer)));check(copy.offset+copy.bytes<=bytes.size(),"actual R8 copy is within its mapped staging allocation");self->imageBytes[number(copy.image)]={bytes.begin()+copy.offset,bytes.begin()+copy.offset+copy.bytes};}for(const auto& update:self->commands[number(cb)]){auto& bytes=self->memory.at(self->boundMemory.at(update.buffer));if(update.fill){check(self->bufferSizes.at(update.buffer)<=bytes.size(),"actual shared UBO fits queried allocation");std::memset(bytes.data(),0,std::size_t(self->bufferSizes.at(update.buffer)));}else{check(update.offset+4<=bytes.size(),"guard update inside allocation");std::memcpy(bytes.data()+update.offset,&update.value,4);}}auto app=self->appControl.find(number(cb));if(app!=self->appControl.end()){float v=0;auto& bytes=self->memory.at(self->boundMemory.at(app->second));std::memcpy(&v,bytes.data()+self->appOffset.at(number(cb)),4);self->reads.push_back(v);if(v==1)for(auto set:self->appMasks[number(cb)])self->sampledMasks.push_back(self->imageBytes.at(self->viewImage.at(self->coverageView.at(number(set)))));}}
 static VkResult VKAPI_CALL submit(VkQueue queue,std::uint32_t n,const VkSubmitInfo* ci,VkFence fence){auto& s=*self;s.forwarded.push_back({queue,n,ci,fence,false});++s.calls;++s.submits;check(queue==s.queue,"admitted original queue");s.allSubmitFences.push_back(fence);bool original=false;
  for(unsigned i=0;i<n;++i){if(ci[i].pNext)s.allForwardedExtensions.push_back(ci[i].pNext);if(ci[i].waitSemaphoreCount){check(ci[i].pWaitSemaphores&&*ci[i].pWaitSemaphores==s.wait&&ci[i].pWaitDstStageMask&&*ci[i].pWaitDstStageMask==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,"original wait semaphore/stage retained");++s.waitPreserved;}
   if(ci[i].signalSemaphoreCount){check(ci[i].pSignalSemaphores&&*ci[i].pSignalSemaphores==s.signalSemaphore,"original signal retained");++s.signalPreserved;}
   for(unsigned k=0;k<ci[i].commandBufferCount;++k){original|=s.appControl.count(number(ci[i].pCommandBuffers[k]))||s.ordinaryMarks.count(number(ci[i].pCommandBuffers[k]));execute(ci[i].pCommandBuffers[k]);}}
  if(original){s.lastSubmitInfos.assign(ci,ci+n);s.lastSubmitCommands.clear();for(unsigned i=0;i<n;++i)if(ci[i].commandBufferCount)s.lastSubmitCommands.insert(s.lastSubmitCommands.end(),ci[i].pCommandBuffers,ci[i].pCommandBuffers+ci[i].commandBufferCount);for(unsigned i=0;i<n;++i)for(unsigned k=0;k<ci[i].commandBufferCount;++k)if(s.appControl.count(number(ci[i].pCommandBuffers[k])))s.originalNexts.push_back(ci[i].pNext);++s.forwardOriginal;s.originalFences.push_back(fence);s.nativeSubmitCounts.push_back(n);if(s.afterOriginal)s.afterOriginal();if(s.failOriginal)return VK_ERROR_DEVICE_LOST;}
  if(fence){if(s.autoSignal)s.fences[number(fence)]=true;else s.pending.push_back(number(fence));}return VK_SUCCESS;
 }
 static VkResult VKAPI_CALL submit2(VkQueue q,std::uint32_t n,const VkSubmitInfo2* ci,VkFence fence){self->forwarded.push_back({q,n,ci,fence,true});bool original=false;for(unsigned i=0;i<n;++i)for(unsigned k=0;k<ci[i].commandBufferInfoCount;++k){original|=self->appControl.count(number(ci[i].pCommandBufferInfos[k].commandBuffer));execute(ci[i].pCommandBufferInfos[k].commandBuffer);}if(original&&self->afterOriginal)self->afterOriginal();if(fence){if(self->autoSignal)self->fences[number(fence)]=true;else self->pending.push_back(number(fence));}check(q==self->queue,"submit2 exact queue");return self->failOriginal?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
 static VkResult VKAPI_CALL submit2KHR(VkQueue q,std::uint32_t n,const VkSubmitInfo2* ci,VkFence fence){++self->khrCalls;return submit2(q,n,ci,fence);}
 static PFN_vkVoidFunction VKAPI_CALL get(VkDevice d,const char* n){
#define FN(Name,Body) if(std::strcmp(n,"vk" #Name)==0)return reinterpret_cast<PFN_vkVoidFunction>(&Body);
  FN(BeginCommandBuffer,beginCommand) FN(CreateBuffer,createBuffer) FN(GetBufferMemoryRequirements,bufferReq) FN(CmdFillBuffer,fillBuffer) FN(CmdUpdateBuffer,bufferUpdate) FN(CmdPipelineBarrier,barrier) FN(BindBufferMemory,bind) FN(UpdateDescriptorSets,update) FN(CreateImageView,createView) FN(CmdCopyBufferToImage,copyImage)
  FN(CreateShaderModule,createShader) FN(DestroyShaderModule,destroyShader) FN(CreateGraphicsPipelines,createPipelines) FN(DestroyPipeline,destroyPipeline) FN(CreatePipelineLayout,createLayout) FN(DestroyPipelineLayout,destroyLayout) FN(FreeDescriptorSets,freeSets) FN(DeviceWaitIdle,idle) FN(GetFenceStatus,fenceStatus) FN(QueueSubmit,submit) FN(QueueSubmit2,submit2) FN(QueueSubmit2KHR,submit2KHR)
#undef FN
  return Fake::get(d,n);
 }
};Driver* Driver::self=nullptr;
}
namespace stream_bridge_renderer {
bool prepareProcessCold()noexcept{return true;}bool enabled()noexcept{return true;}ProcessKind processKind()noexcept{return ProcessKind::Compositor;}
stream_bridge_native::NativeImages* sourceImages()noexcept{return nullptr;}void sourceExportedFd(int)noexcept{}
bool registerConsumerCold(ProcessConsumer c)noexcept{consumer=c;return true;}void unregisterConsumerCold(void*)noexcept{consumer={};}
bool readCoverageFragmentCold(std::vector<std::uint32_t>& out)noexcept{if(!coverageReadable)return false;out=coverageWords;return true;}
}
namespace stream_bridge_local {
struct LeaseReader::Impl{};LeaseReader::LeaseReader():impl_(new Impl){}LeaseReader::~LeaseReader()=default;
bool LeaseReader::mapReadOnly(int)noexcept{return true;}bool LeaseReader::current(const LeaseReference& v)const noexcept{return leaseCurrent&&v.sequence==leaseSequence;}void LeaseReader::reset()noexcept{}
}
namespace stream_bridge_native {
struct NativeImages::Impl{};NativeImages::NativeImages():impl_(new Impl){}NativeImages::NativeImages(ExportPreparation,void*):NativeImages(){}NativeImages::~NativeImages()=default;
bool NativeImages::created(DeviceHandle,ImageHandle,const ImageDescription&)noexcept{return false;}bool NativeImages::allocated(DeviceHandle,MemoryHandle,bool)noexcept{return false;}bool NativeImages::bound(DeviceHandle,ImageHandle,MemoryHandle,std::uint64_t)noexcept{return false;}void NativeImages::destroyed(DeviceHandle,ImageHandle)noexcept{}void NativeImages::freed(DeviceHandle,MemoryHandle)noexcept{}bool NativeImages::exported(DeviceHandle,MemoryHandle,int,std::uint32_t,std::uintptr_t)noexcept{return false;}
}
namespace {
struct Integration {
 Driver vk;sr::Renderer renderer;CloneFixture native;
 std::array<VkDescriptorSetLayoutBinding,29> observedBindings{};
 VkPipeline publicPipeline=handle<VkPipeline>(0x60);
 VkDevice device=handle<VkDevice>(1);VkCommandBuffer cb=handle<VkCommandBuffer>(0x71000);VkImage image=handle<VkImage>(0x72000);VkImageView view=handle<VkImageView>(0x73000);VkDescriptorSet set=handle<VkDescriptorSet>(0x74000);VkDeviceMemory imported=handle<VkDeviceMemory>(0x75000);
 std::shared_ptr<std::atomic<bool>> connection=std::make_shared<std::atomic<bool>>(true);int file=-1;sr::LocalPicture picture;VkBuffer guard=VK_NULL_HANDLE;
 Integration(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps,unsigned imageCount=3,bool useColdProof=true,bool expectReady=true,bool failCoverage=false,bool customAllocator=false,bool prime=true,unsigned pendingPipelines=0){
  leaseCurrent=true;leaseSequence=71;vk.fences[number(vk.appFence)]=false;float priority=1;VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queueInfo.queueFamilyIndex=0;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;native.device.queueCreateInfoCount=1;native.device.pQueueCreateInfos=&queueInfo;
  VkPhysicalDeviceMemoryProperties m{};m.memoryTypeCount=1;m.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  check(renderer.prepareCold(device,m,Driver::get,native.device,VK_API_VERSION_1_1,0)==coverageReadable,"actual renderer cold prepare requires only exact coverage artifact");check(!consumer.owner,"pool preparation alone cannot advertise actual selected-image renderer");
  if(imageCount==1){
   // The retained native archive has ALL-stage scalar bindings 0..28, not
   // the old convenient seven-binding array3 fixture.
   for(unsigned n=0;n<observedBindings.size();++n)observedBindings[n]={n,n<8?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:n==8?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:n<25?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_ALL,nullptr};
   native.set0.bindingCount=observedBindings.size();native.set0.pBindings=observedBindings.data();
  }
  auto& definitions=renderer.definitions();check(definitions.createSet(native.nativeSet,native.set0,VK_SUCCESS),"actual native set definitions");check(definitions.createLayout(native.graphics.layout,native.layout,VK_SUCCESS),"actual native layout definitions");check(definitions.createRenderPass(native.graphics.renderPass,native.pass,VK_SUCCESS),"actual native renderpass definitions");
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=vs.size()*4;shader.pCode=vs.data();renderer.shaderCreated(native.stages[1].module,shader);shader.codeSize=ps.size()*4;shader.pCode=ps.data();
  for(unsigned n=0;n<pendingPipelines;++n)renderer.graphicsCreated(VK_NULL_HANDLE,native.graphics,nullptr,publicPipeline);
  if(useColdProof)renderer.shaderCreated(native.stages[0].module,shader);
  else check(definitions.createShader(native.stages[0].module,shader,VK_SUCCESS),"unpromoted exact native FS recorded without interface authority");
  native.colorBlend.blendEnable=VK_FALSE;
  vk.failCoverage=failCoverage;VkAllocationCallbacks unsupportedAllocator{};
  renderer.graphicsCreated(VK_NULL_HANDLE,native.graphics,customAllocator?&unsupportedAllocator:nullptr,publicPipeline);
  if(!expectReady){check(!consumer.owner,"pending or unpinned native count1 stays unadmitted");return;}
  check(consumer.owner&&consumer.coverage&&consumer.picture&&consumer.tick,"only actual exact-coverage native companion registers receiver route");
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.pNext=&external;info.imageType=VK_IMAGE_TYPE_2D;info.extent={16,2,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.format=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;renderer.imageCreated(image,info);
  char path[]="/tmp/sfnp-renderer-fixture-XXXXXX";file=mkstemp(path);check(file>=0,"owned kernel-object fixture fd");unlink(path);struct stat st{};check(!fstat(file,&st),"exact kernel object observed");VkImportMemoryFdInfoKHR fd{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};fd.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;fd.fd=file;VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.pNext=&fd;allocate.allocationSize=32;auto preparation=renderer.memoryPreparing(allocate);check(preparation.duplicate>=0,"fd duplicated BEFORE original consumes it");renderer.memoryAllocated(imported,allocate,preparation,VK_SUCCESS);renderer.imageBound(image,imported,0);
  VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};renderer.viewCreated(view,ci);writeView(view);
  picture.metadata.kind=local::Kind::Decoded;picture.metadata.descriptor={7,9,16,2};picture.metadata.binding[0]=31;picture.metadata.lease.kernel={std::uint64_t(st.st_dev),std::uint64_t(st.st_ino)};picture.metadata.lease.sequence=71;picture.metadata.peerConfirmed=true;picture.leases=std::make_shared<local::LeaseReader>();picture.connection=connection;
  renderer.commandAllocated(handle<VkCommandBuffer>(0x70fff),VK_COMMAND_BUFFER_LEVEL_PRIMARY,0);renderer.commandAllocated(cb,VK_COMMAND_BUFFER_LEVEL_PRIMARY,0);
  if(prime){publish();consumer.picture(consumer.owner,picture);
  // Legacy warmed cases explicitly create then discard a guarded recording.
  // They are not startup-idle cases; their later zero/retirement path is live.
  check(record().ready,"warmed fixture explicitly records an exact pending mask");renderer.commandReset(cb);vk.appControl.erase(number(cb));vk.appMasks.erase(number(cb));
  // This convenience path completes a cold upload. First-use scheduling is
  // independently exercised below without signalling its upload fence.
  renderer.submit(vk.queue,0,nullptr,VK_NULL_HANDLE);vk.signal();consumer.tick(consumer.owner);}
 }
 ~Integration(){renderer.stopCold();if(file>=0)close(file);}
 void publish(sb::CoverageEncoding encoding=sb::CoverageEncoding::Raw,unsigned size=32){sb::CertifiedAccessUnit certificate;certificate.descriptor=picture.metadata.descriptor;certificate.binding=picture.metadata.binding;certificate.coverageEncoding=encoding;certificate.coverage.assign(encoding==sb::CoverageEncoding::None?0:size,127);consumer.coverage(consumer.owner,certificate);}
 void writeView(VkImageView selected){VkDescriptorImageInfo imageInfo{};imageInfo.imageView=selected;imageInfo.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=9;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.descriptorCount=1;write.pImageInfo=&imageInfo;renderer.descriptorWrites(1,&write,0,nullptr);}
 sr::Draw record(VkCommandBufferUsageFlags flags=0){VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=flags;renderer.commandBegin(cb,&begin);renderer.pipelineBound(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,publicPipeline);renderer.setsBound(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,0,1,&set,0);auto result=renderer.beforeDraw(cb);if(result.control){guard=handle<VkBuffer>(vk.setBuffer.at(number(result.control)));vk.appControl[number(cb)]=number(guard);vk.appOffset[number(cb)]=vk.setOffset.at(number(result.control));}return result;}
 sr::Draw freshFrame(){
  ++picture.metadata.descriptor.sourceFrame;picture.metadata.lease.sequence=++leaseSequence;
  sb::CertifiedAccessUnit certificate;certificate.descriptor=picture.metadata.descriptor;certificate.binding=picture.metadata.binding;certificate.coverageEncoding=sb::CoverageEncoding::Raw;certificate.coverage.assign(32,127);
  consumer.coverage(consumer.owner,certificate);consumer.picture(consumer.owner,picture);
  renderer.submit(vk.queue,0,nullptr,VK_NULL_HANDLE);vk.signal();consumer.tick(consumer.owner);
  // Keep the earlier native recording alive: its immutable descriptor storage
  // must remain retained even though that unsupported submit got only zero.
  cb=handle<VkCommandBuffer>(number(cb)+1);renderer.commandAllocated(cb,VK_COMMAND_BUFFER_LEVEL_PRIMARY,0);
  return record();
 }
 void enableWarmCopies(){
  // A real guarded recording is what ends startup-idle passthrough. Retire
  // this intentionally abandoned recording before the warmed-only sequence.
  consumer.picture(consumer.owner,picture);check(record().ready,"explicit warmup first creates a possible guarded recording");renderer.commandReset(cb);vk.appControl.erase(number(cb));vk.appMasks.erase(number(cb));
 }
 VkResult submit(bool semaphores=true){VkPipelineStageFlags stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&cb;if(semaphores){info.waitSemaphoreCount=info.signalSemaphoreCount=1;info.pWaitSemaphores=&vk.wait;info.pWaitDstStageMask=&stage;info.pSignalSemaphores=&vk.signalSemaphore;}return renderer.submit(vk.queue,1,&info,vk.appFence);}
};
void idleStartup(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 for(unsigned mode=0;mode<3;++mode){Integration f(vs,ps,1,true,true,false,false,false);
  f.vk.ordinaryMarks[number(f.cb)]=0x5a;
  VkProtectedSubmitInfo extension{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};extension.protectedSubmit=VK_FALSE;
  VkPipelineStageFlags stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&extension;info.commandBufferCount=1;info.pCommandBuffers=&f.cb;info.waitSemaphoreCount=info.signalSemaphoreCount=1;info.pWaitSemaphores=&f.vk.wait;info.pWaitDstStageMask=&stage;info.pSignalSemaphores=&f.vk.signalSemaphore;
  VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.cb;cb.deviceMask=1;
  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};wait.semaphore=f.vk.wait;wait.stageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;wait.value=17;
  VkSemaphoreSubmitInfo signal=wait;signal.semaphore=f.vk.signalSemaphore;signal.value=19;
  VkSubmitInfo2 info2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info2.pNext=&extension;info2.commandBufferInfoCount=1;info2.pCommandBufferInfos=&cb;info2.waitSemaphoreInfoCount=info2.signalSemaphoreInfoCount=1;info2.pWaitSemaphoreInfos=&wait;info2.pSignalSemaphoreInfos=&signal;
  const auto call=[&]{return mode?f.renderer.submit2(f.vk.queue,1,&info2,f.vk.appFence,mode==2):f.renderer.submit(f.vk.queue,1,&info,f.vk.appFence);};
  const auto forward=[&](std::size_t before){check(f.vk.forwarded.size()==before+1,"idle startup forwards precisely one native queue call");const auto& r=f.vk.forwarded.back();check(r.queue==f.vk.queue&&r.count==1&&r.infos==(mode?static_cast<const void*>(&info2):static_cast<const void*>(&info))&&r.fence==f.vk.appFence&&r.sync2==bool(mode),"idle startup forwards original submit pointer/count/queue/fence and dispatch unchanged");};
  const auto before=f.vk.forwarded.size(),executed=f.vk.executedCommands.size();const auto kh=f.vk.khrCalls;
  check(call()==VK_SUCCESS,"idle native queue result preserved");forward(before);
  check(f.vk.executedCommands.size()==executed+1&&f.vk.executedCommands.back()==f.cb&&f.vk.ordinaryExecuted.back()==0x5a,"idle startup executes only unchanged native content, no owned zero/copy/guard command");
  check(f.vk.khrCalls==kh+unsigned(mode==2),"idle KHR path keeps its distinct native function");
  f.vk.failOriginal=true;const auto failed=f.vk.forwarded.size();check(call()==VK_ERROR_DEVICE_LOST,"idle native queue error returned unchanged");forward(failed);f.vk.failOriginal=false;
  auto stats=f.renderer.statsCold();check(!stats.metadata&&!stats.pictures&&!stats.selected&&!stats.coreSubmits&&!stats.submits2&&!stats.guardAttempts&&!stats.opaqueSubmits,"idle startup does not enter mask dispatch or claim opaque fallback activity");
  f.publish();const auto pending=f.vk.forwarded.size();check(call()==VK_SUCCESS,"unmatched pending mask does not modify idle native route");forward(pending);
  stats=f.renderer.statsCold();check(stats.pending==1&&!stats.copying&&!stats.ready,"metadata without a guarded recording cannot enqueue GPU uploads on idle native submissions");
  consumer.picture(consumer.owner,f.picture);const auto draw=f.record();check(draw.ready,"first exact pending picture can create guarded recording without a startup GPU prewarm");f.vk.appMasks[number(f.cb)]={draw.coverage};
  check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1&&f.vk.sampledMasks.back()==std::vector<std::uint8_t>(32,127),"first actual guarded submit enqueues exact copy and samples correct current coverage");
  f.connection->store(false);check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"after first coverage, disabled source must zero a reused flags0 recording rather than return to idle bypass");
  f.connection->store(true);check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"still-current exact source can restore coverage before independent tracking loss");
  f.renderer.trackingLost();VkSubmitInfo2 replay{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};replay.commandBufferInfoCount=1;replay.pCommandBufferInfos=&cb;
  check(f.renderer.submit2(f.vk.queue,1,&replay,f.vk.appFence,mode==2)==VK_SUCCESS&&f.vk.reads.back()==0,"tracking loss retains mandatory zeroing for previously guarded Submit2 recordings");
 }
 {Integration f(vs,ps,1,true,true,false,false,false);f.vk.ordinaryMarks[number(f.cb)]=0x7b;f.vk.blockPipeline=true;VkPipeline other=handle<VkPipeline>(0x61);std::thread cold([&]{f.renderer.graphicsCreated(VK_NULL_HANDLE,f.native.graphics,nullptr,other);});const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!f.vk.insidePipeline.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();const bool locked=f.vk.insidePipeline.load();const auto before=f.vk.forwarded.size();const auto result=f.submit(false);f.vk.blockPipeline=false;cold.join();check(locked,"idle contention control reaches actual native pipeline creation");check(result==VK_SUCCESS&&f.vk.forwarded.size()==before+1&&f.vk.forwarded.back().count==1,"idle native submit stays transparent even while actual cold pipeline preparation holds renderer mutex");}
}
void telemetry(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 {Integration f(vs,ps,1,true,true,false,false,false,64);check(f.renderer.statsCold().pipelines==1,"repeated pending startup pipelines do not prevent later exact admission");}
 {Integration f(vs,ps,3,true,true,false,false,false);
  f.publish(sb::CoverageEncoding::None);auto s=f.renderer.statsCold();
  check(s.metadata==1&&s.maskNone==1&&!s.maskDecoded&&!s.uploadAccepted&&!s.poolUploaded,"maskless discovery metadata cannot count as a decoded or uploaded mask");
  f.publish(sb::CoverageEncoding::Raw,31);s=f.renderer.statsCold();
  check(s.metadata==2&&s.maskRejected==1&&!s.maskDecoded&&!s.poolUploaded,"invalid authenticated mask bytes have a distinct decode refusal");
  f.publish();consumer.picture(consumer.owner,f.picture);consumer.picture(consumer.owner,f.picture);s=f.renderer.statsCold();
  check(s.maskDecoded==1&&s.uploadAccepted==1&&s.poolUploaded==1&&s.pending==1&&!s.copying&&!s.ready&&s.pictures==1,"decoded accepted pending upload and unique delivered picture are separate facts");
  const auto selected=f.record();check(selected.ready,"exact first-use pending mask can be recorded before its same-queue copy");s=f.renderer.statsCold();
  check(s.pending==1&&!s.copying&&!s.ready&&s.selected==1&&!s.guardAttempts&&!s.guardAccepted,"scheduled recording is neither completed storage nor successful display");
  f.vk.appMasks[number(f.cb)]={selected.coverage};
  check(f.submit(false)==VK_SUCCESS,"native submit first enqueues actual R8 copy and then guarded sampling");s=f.renderer.statsCold();
  check(!s.pending&&s.copying==1&&!s.ready&&s.guardAttempts==1&&s.guardAccepted==1&&!s.guardRefused,"copying on the original queue permits exact first sampling without a CPU fence wait");
  check(f.vk.sampledMasks.size()==1&&f.vk.sampledMasks.back()==std::vector<std::uint8_t>(32,127),"first native draw samples its own newly copied bytes, never uninitialized storage");
  f.vk.signal();consumer.tick(consumer.owner);s=f.renderer.statsCold();
  check(s.ready==1&&s.selected==1&&s.guardAccepted==1,"later fence reap records completion without inventing another presentation");
  f.vk.failOriginal=true;check(f.submit(false)==VK_ERROR_DEVICE_LOST,"actual native submit refusal remains unchanged");s=f.renderer.statsCold();
  check(s.guardAttempts==2&&s.guardAccepted==1&&s.guardRefused==1&&s.quarantined==1,"failed original submit is distinct and genuine GPU quarantine remains present");
 }
 {Integration f(vs,ps,3,true,true,false,false,false);
  check(!f.record().ready,"legal early native recording has no speculative image or mask");
  f.publish();consumer.picture(consumer.owner,f.picture);f.renderer.submit(f.vk.queue,0,nullptr,VK_NULL_HANDLE);f.vk.signal();consumer.tick(consumer.owner);
  check(f.submit(false)==VK_SUCCESS&&f.vk.reads.empty(),"known limit cached early CB stays native when the correct mask arrives later");
  auto s=f.renderer.statsCold();check(s.pending==1&&!s.ready&&s.pictures==1&&s.draws==1&&!s.selected&&!s.guardAccepted&&s.drawRefusals[std::size_t(sr::DrawRefusal::ExactPicture)]==1,"telemetry exposes unsubmitted pending mask plus early-record miss without inventing a startup GPU copy or late bind");
  check(f.record().ready&&f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"actual native re-record is still necessary and restores current exact coverage");
  for(unsigned n=0;n<4096;++n)consumer.tick(consumer.owner); // Unchanged cold snapshots emit no repeated status.
 }
 {Integration f(vs,ps);f.renderer.beforeDraw(handle<VkCommandBuffer>(0xdead));f.renderer.beforeDraw(handle<VkCommandBuffer>(0x70fff));f.record(VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
  f.record();f.renderer.pipelineBound(f.cb,VK_PIPELINE_BIND_POINT_GRAPHICS,handle<VkPipeline>(0xbeef));f.renderer.beforeDraw(f.cb);
  f.record();f.writeView(handle<VkImageView>(0xdead));f.renderer.beforeDraw(f.cb);
  auto s=f.renderer.statsCold();check(s.drawRefusals[std::size_t(sr::DrawRefusal::Command)]==1&&s.drawRefusals[std::size_t(sr::DrawRefusal::Epoch)]==1&&s.drawRefusals[std::size_t(sr::DrawRefusal::Ineligible)]==1&&s.drawRefusals[std::size_t(sr::DrawRefusal::Pipeline)]==1&&s.drawRefusals[std::size_t(sr::DrawRefusal::Descriptor)]==1,"unknown CB epoch pipeline eligibility and descriptor misses remain distinct");
  f.writeView(f.view);f.renderer.imageDestroyed(f.image);f.renderer.beforeDraw(f.cb);s=f.renderer.statsCold();check(s.drawRefusals[std::size_t(sr::DrawRefusal::Image)]==1,"retired imported image has a distinct draw refusal");
 }
}
void continuous(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 for(unsigned mode=0;mode<2;++mode){Integration f(vs,ps,1,true,true,false,false,false);
  const auto publish=[&](bool active,std::uint8_t marker){sb::CertifiedAccessUnit certificate;certificate.descriptor=f.picture.metadata.descriptor;certificate.binding=f.picture.metadata.binding;certificate.coverageEncoding=active?sb::CoverageEncoding::Raw:sb::CoverageEncoding::None;if(active)certificate.coverage.assign(32,marker);consumer.coverage(consumer.owner,certificate);};
  // Cold startup prewarms every finite slot through actual queue copy/fence
  // completion. The tested active stream never relies on a first-use image.
  for(unsigned n=0;n<stream_bridge_coverage_gpu::MaximumSlots;++n){++f.picture.metadata.descriptor.sourceFrame;publish(true,static_cast<std::uint8_t>(n+1));if(!n)f.enableWarmCopies();f.renderer.submit(f.vk.queue,0,nullptr,VK_NULL_HANDLE);f.vk.signal();consumer.tick(consumer.owner);}
  check(f.renderer.statsCold().ready==stream_bridge_coverage_gpu::MaximumSlots,"continuous stream starts with every finite image actually initialized");const auto allocations=f.vk.allocations;unsigned activeFrames=0,inactiveFrames=0;
  for(unsigned frame=0;frame<128;++frame){const bool active=!mode||frame%11!=3;const auto marker=static_cast<std::uint8_t>((frame*37)%253+1);
   ++f.picture.metadata.descriptor.sourceFrame;f.picture.metadata.lease.sequence=++leaseSequence;publish(active,marker);consumer.picture(consumer.owner,f.picture);
   f.vk.appControl.erase(number(f.cb));f.vk.appMasks.erase(number(f.cb));
   const auto left=f.record();const auto right=f.renderer.beforeDraw(f.cb);
   check(left.ready==active&&right.ready==active,"every active exact frame records both eyes; explicit inactive source records neither");
   if(active){f.vk.appMasks[number(f.cb)]={left.coverage,right.coverage};++activeFrames;}else ++inactiveFrames;
   const auto samples=f.vk.sampledMasks.size();const auto commands=std::count(f.vk.executedCommands.begin(),f.vk.executedCommands.end(),f.cb);const auto guards=f.renderer.statsCold().guardAccepted;
   check(f.submit(false)==VK_SUCCESS,"continuous original native frame result is preserved");
   check(std::count(f.vk.executedCommands.begin(),f.vk.executedCommands.end(),f.cb)==commands+1,"each native frame executes exactly once in original queue order");
   check(f.renderer.statsCold().guardAccepted==guards+unsigned(active)&&f.vk.sampledMasks.size()==samples+2*unsigned(active),"only active exact frame enables its two eye samples");
   if(active)for(std::size_t eye=0;eye<2;++eye)check(f.vk.sampledMasks[samples+eye]==std::vector<std::uint8_t>(32,marker),"both native eye draws sample this frame's changed R8 bytes, never a previous mask");
   f.vk.signal();consumer.tick(consumer.owner);
   if(frame%7==5){const auto repeated=f.vk.sampledMasks.size();check(f.submit(false)==VK_SUCCESS,"same still-current source supports legal flags0 repeat");check(f.vk.sampledMasks.size()==repeated+2*unsigned(active),"repeat retains exact active/inactive source instead of inventing another mask");if(active)for(std::size_t eye=0;eye<2;++eye)check(f.vk.sampledMasks[repeated+eye]==std::vector<std::uint8_t>(32,marker),"repeated native frame samples its own retained exact mask");f.vk.signal();consumer.tick(consumer.owner);}
   const auto stats=f.renderer.statsCold();check(!stats.quarantined&&!stats.uploadRefused&&!stats.guardRefused,"continuous finite-pool reuse loses no active frame to quarantine or reservation refusal");
  }
  check(activeFrames+inactiveFrames==128&&(!mode?inactiveFrames==0:inactiveFrames>0),"all-ready active stream and explicit inactive intervals are independent controls");check(f.vk.allocations==allocations,"long stream reuses bounded initialized image storage without per-frame GPU allocation");
 }
 // Keep the previous real tail-fence poll blocked through this COMPLETE
 // new frame: record, pending copy, reservation, native submit and tail fence.
 for(unsigned mode=0;mode<3;++mode){Integration f(vs,ps,1,true,true,false,false,false);
  const auto publish=[&](std::uint8_t marker){sb::CertifiedAccessUnit certificate;certificate.descriptor=f.picture.metadata.descriptor;certificate.binding=f.picture.metadata.binding;certificate.coverageEncoding=sb::CoverageEncoding::Raw;certificate.coverage.assign(32,marker);consumer.coverage(consumer.owner,certificate);};
  for(unsigned n=0;n<stream_bridge_coverage_gpu::MaximumSlots;++n){++f.picture.metadata.descriptor.sourceFrame;publish(static_cast<std::uint8_t>(n+1));if(!n)f.enableWarmCopies();f.renderer.submit(f.vk.queue,0,nullptr,VK_NULL_HANDLE);f.vk.signal();consumer.tick(consumer.owner);}
  consumer.picture(consumer.owner,f.picture);check(f.record().ready&&f.submit(false)==VK_SUCCESS,"cold-contention sequence has a genuine previous guarded submit");f.vk.signal();
  ++f.picture.metadata.descriptor.sourceFrame;f.picture.metadata.lease.sequence=++leaseSequence;publish(201);consumer.picture(consumer.owner,f.picture);
  check(f.renderer.statsCold().pending==1,"new exact marker is pending before cold-worker contention");
  const auto samples=f.vk.sampledMasks.size();const auto previous=f.renderer.statsCold().guardAccepted;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};f.renderer.commandBegin(f.cb,&begin);f.renderer.pipelineBound(f.cb,VK_PIPELINE_BIND_POINT_GRAPHICS,f.publicPipeline);f.renderer.setsBound(f.cb,VK_PIPELINE_BIND_POINT_GRAPHICS,0,1,&f.set,0);
  f.vk.appControl.erase(number(f.cb));f.vk.blockFenceStatus=true;std::thread cold([&]{consumer.tick(consumer.owner);});while(!f.vk.insideFenceStatus.load())std::this_thread::yield();
  const auto left=f.renderer.beforeDraw(f.cb),right=f.renderer.beforeDraw(f.cb);
  if(left.ready&&right.ready){f.vk.appControl[number(f.cb)]=f.vk.setBuffer.at(number(left.control));f.vk.appOffset[number(f.cb)]=f.vk.setOffset.at(number(left.control));f.vk.appMasks[number(f.cb)]={left.coverage,right.coverage};}
  VkResult result;
  if(!mode)result=f.submit(false);
  else{VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.cb;VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;result=f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence,mode==2);}
  f.vk.blockFenceStatus=false;cold.join();
  check(left.ready&&right.ready,"an already published exact mask remains recordable while cold fence status is blocked");
  check(result==VK_SUCCESS&&f.renderer.statsCold().guardAccepted==previous+1,"cold fence poll cannot interrupt exact copy/reservation/native/tail submission");
  check(f.vk.sampledMasks.size()==samples+2&&f.vk.sampledMasks[samples]==std::vector<std::uint8_t>(32,201)&&f.vk.sampledMasks[samples+1]==std::vector<std::uint8_t>(32,201),"complete blocked-worker frame samples its new exact R8 bytes in both eyes");
  f.vk.signal();consumer.tick(consumer.owner);const auto stats=f.renderer.statsCold();check(!stats.quarantined&&!stats.guardRefused&&!stats.uploadRefused&&stats.ready==stream_bridge_coverage_gpu::MaximumSlots,"complete blocked-worker cycle retires normally without lost pool slots");
 }

}
void reservationRecovery(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 for(unsigned mode=0;mode<6;++mode){Integration f(vs,ps);check(f.record().ready,"reserve initial current exact picture");check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"first real guarded submit owns a retirement ticket");f.vk.signal();
  check(f.record().ready,"native completed recording still selects this exact current picture");
  f.vk.blockFenceStatus=true;std::thread cold;
  const auto startCold=[&]{cold=std::thread([&]{consumer.tick(consumer.owner);});while(!f.vk.insideFenceStatus.load())std::this_thread::yield();};
  if(mode<3)startCold();else f.vk.afterOriginal=startCold;
  VkResult result;
  if(mode%3==0)result=f.submit(false);
  else{VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.cb;VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;result=f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence,mode%3==2);}
  f.vk.blockFenceStatus=false;cold.join();
  f.vk.afterOriginal={};
  check(result==VK_SUCCESS&&f.vk.reads.back()==1,"cold fence contention preserves the exact native guard before and after reservation without waiting");
  const auto stats=f.renderer.statsCold();check(stats.quarantined==0,mode<3?"pre-submit reservation refusal cannot permanently quarantine a mask never enabled on GPU":"post-submit cold contention cannot lose its already reserved exact retirement ticket");
  f.vk.signal();consumer.tick(consumer.owner);
  check(f.record().ready&&f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"the same exact current picture recovers immediately after reservation contention");
 }
}
void timeline(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 for(unsigned mode=0;mode<9;++mode){Integration f(vs,ps);check(f.record().ready,"timeline case records an exact current picture");
  const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;const std::uint64_t waitValue=17,signalValue=29;
  VkTimelineSemaphoreSubmitInfo extension{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};extension.waitSemaphoreValueCount=extension.signalSemaphoreValueCount=1;extension.pWaitSemaphoreValues=&waitValue;extension.pSignalSemaphoreValues=&signalValue;
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&extension;info.commandBufferCount=1;info.pCommandBuffers=&f.cb;info.waitSemaphoreCount=info.signalSemaphoreCount=1;info.pWaitSemaphores=&f.vk.wait;info.pWaitDstStageMask=&stage;info.pSignalSemaphores=&f.vk.signalSemaphore;
  VkProtectedSubmitInfo tail{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};
  if(mode==1){extension.waitSemaphoreValueCount=extension.signalSemaphoreValueCount=0;extension.pWaitSemaphoreValues=extension.pSignalSemaphoreValues=nullptr;info.waitSemaphoreCount=info.signalSemaphoreCount=0;info.pWaitSemaphores=info.pSignalSemaphores=nullptr;info.pWaitDstStageMask=nullptr;}
  if(mode==2)extension.waitSemaphoreValueCount=0;
  if(mode==3)extension.signalSemaphoreValueCount=0;
  if(mode==4)extension.pWaitSemaphoreValues=nullptr;
  if(mode==5)extension.pSignalSemaphoreValues=nullptr;
  if(mode==6)extension.pNext=&tail;
  if(mode==7)extension.sType=VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;
  if(mode==8)extension.sType=static_cast<VkStructureType>(0x7ffffffe);
  const auto original=info;const auto originalExtension=extension;
  check(f.renderer.submit(f.vk.queue,1,&info,f.vk.appFence)==VK_SUCCESS,"timeline call preserves original native result");
  const bool accepted=mode<2;
  check(f.vk.reads.back()==float(accepted),"canonical timeline retains current exact guard; malformed or unknown extension stays opaque");
  check(f.renderer.statsCold().guardAccepted==unsigned(accepted),"canonical timeline reaches actual guarded submission rather than merely selection");
  const auto stats=f.renderer.statsCold();
  check(stats.firstNextType==static_cast<std::uint32_t>(extension.sType)&&stats.firstRefusedNextType==(accepted?0:static_cast<std::uint32_t>(extension.sType)),"cold diagnostics retain actual first native extension and refusal types");
  check(stats.collects==unsigned(accepted)&&stats.collected==unsigned(accepted),"unsupported timeline is distinguished from selected draw failing pre-submit collection");
  check(stats.submitRefusals[std::size_t(sr::SubmitRefusal::Shape)]==unsigned(mode>=2&&mode<=5)&&stats.submitRefusals[std::size_t(sr::SubmitRefusal::Extension)]==unsigned(mode>=6),"malformed timeline shape and unsupported extension have distinct bounded counters");
  check(std::memcmp(&original,&info,sizeof info)==0&&std::memcmp(&originalExtension,&extension,sizeof extension)==0&&waitValue==17&&signalValue==29,"original timeline arrays values counts chain and submit remain byte-identical");
  check(f.vk.lastSubmitInfos.size()==2,"zero guard precedes exactly one original native timeline submit");
  const auto& zero=f.vk.lastSubmitInfos[0];const auto& forwarded=f.vk.lastSubmitInfos[1];
  check(!zero.pNext&&!zero.waitSemaphoreCount&&!zero.signalSemaphoreCount&&zero.commandBufferCount==1,"synthetic zero has no inherited timeline values or semaphores");
  check(forwarded.pNext==original.pNext&&forwarded.waitSemaphoreCount==original.waitSemaphoreCount&&forwarded.signalSemaphoreCount==original.signalSemaphoreCount&&forwarded.pWaitSemaphores==original.pWaitSemaphores&&forwarded.pWaitDstStageMask==original.pWaitDstStageMask&&forwarded.pSignalSemaphores==original.pSignalSemaphores,"native timeline pointer and all semaphore metadata forwarded exactly");
  check(forwarded.commandBufferCount==unsigned(accepted?2:1)&&f.vk.lastSubmitCommands.back()==f.cb&&f.vk.originalFences.back()==f.vk.appFence,"guard only changes native command list preserving native order and original fence");
  f.freshFrame();check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"timeline refusal does not poison subsequent fresh exact coverage");
 }
 // A valid extension never bypasses the actual submit-time source/CB/lease join.
 for(unsigned mode=0;mode<5;++mode){Integration f(vs,ps);check(f.record().ready,"collect refusal begins with a genuinely selected recording");
  sr::CollectRefusal reason=sr::CollectRefusal::Ineligible;
  if(mode==0)f.renderer.commandUnknown(f.cb);
  if(mode==1){leaseCurrent=false;reason=sr::CollectRefusal::Lease;}
  if(mode==2){f.writeView(handle<VkImageView>(0xdead));reason=sr::CollectRefusal::Descriptor;}
  if(mode==3){f.renderer.trackingLost();reason=sr::CollectRefusal::Epoch;}
  if(mode==4){f.renderer.commandReset(f.cb);reason=sr::CollectRefusal::Empty;}
  VkTimelineSemaphoreSubmitInfo extension{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&extension;info.commandBufferCount=1;info.pCommandBuffers=&f.cb;
  check(f.renderer.submit(f.vk.queue,1,&info,f.vk.appFence)==VK_SUCCESS&&f.vk.reads.back()==0,"canonical timeline still renders opaque when original exact collection fails");
  const auto stats=f.renderer.statsCold();if(!(stats.collects==1&&!stats.collected&&!stats.guardAttempts&&stats.collectRefusals[std::size_t(reason)]==1)){std::cerr<<"collect diagnostic mode="<<mode<<" expected="<<unsigned(reason)<<" attempts="<<stats.collects<<" accepted="<<stats.collected<<" misses=";for(auto n:stats.collectRefusals)std::cerr<<n<<'/';std::cerr<<'\n';}check(stats.collects==1&&!stats.collected&&!stats.guardAttempts&&stats.collectRefusals[std::size_t(reason)]==1,"post-selection collect refusal identifies its exact first invalidation cause");
  consumer.tick(consumer.owner);
 }
 for(unsigned mode=0;mode<3;++mode){Integration f(vs,ps);check(f.record().ready,"Submit2 diagnostic starts selected");VkProtectedSubmitInfo extension{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.cb;VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;
  if(mode<2)info.pNext=&extension;else info.flags=VK_SUBMIT_PROTECTED_BIT;
  check(f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence,mode==1)==VK_SUCCESS&&f.vk.reads.back()==0,"core and KHR Submit2 unsupported flags/extensions remain opaque");const auto stats=f.renderer.statsCold();
  check(stats.submits2==1&&!stats.collects&&!stats.guardAttempts&&stats.submitRefusals[std::size_t(mode<2?sr::SubmitRefusal::Extension:sr::SubmitRefusal::Flags)]==1,"Submit2 reports refusal before collection without changing admission");
  check(stats.firstNextType==unsigned(mode<2?VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:0),"Submit2 reports its actual first native pNext numeric type");consumer.tick(consumer.owner);
 }
}
void fallbackRecovery(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 for(unsigned mode=0;mode<6;++mode){
  Integration f(vs,ps);const auto initial=f.record();check(initial.ready,"initial supported coverage recording");
  if(mode==0){
   VkProtectedSubmitInfo extension{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};extension.protectedSubmit=VK_FALSE;
   VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&extension;info.commandBufferCount=1;info.pCommandBuffers=&f.cb;
   check(f.renderer.submit(f.vk.queue,1,&info,f.vk.appFence)==VK_SUCCESS,"unsupported core pNext forwards successful native submit");
  }else if(mode==1||mode==2){
   VkProtectedSubmitInfo extension{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};extension.protectedSubmit=VK_FALSE;
   VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=f.cb;
   VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.pNext=&extension;info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;
   check(f.renderer.submit2(f.vk.queue,1,&info,f.vk.appFence,mode==2)==VK_SUCCESS,"unsupported core or KHR submit2 forwards successful native submit");
  }else if(mode==3){
   std::array<VkSubmitInfo,129> infos{};for(auto& info:infos)info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
   infos.front().commandBufferCount=1;infos.front().pCommandBuffers=&f.cb;
   check(f.renderer.submit(f.vk.queue,infos.size(),infos.data(),f.vk.appFence)==VK_SUCCESS,"unsupported large batch forwards successful native submit");
  }else if(mode==4){
   // The first submit tentatively collects an exact lease. A later extension
   // must discard every prepared one guard, including this earlier prefix.
   VkProtectedSubmitInfo extension{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};extension.protectedSubmit=VK_FALSE;
   std::array<VkSubmitInfo,2> infos{};for(auto& info:infos)info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
   infos.front().commandBufferCount=1;infos.front().pCommandBuffers=&f.cb;infos.back().pNext=&extension;
   check(f.renderer.submit(f.vk.queue,infos.size(),infos.data(),f.vk.appFence)==VK_SUCCESS,"unsupported suffix forwards original supported prefix without one guard");
  }else{
   // A legal cold pipeline creation can own the renderer mutex while the
   // native queue owner submits. Only that submitted batch may become opaque.
   f.vk.blockPipeline=true;VkPipeline other=handle<VkPipeline>(0x61);
   std::thread cold([&]{f.renderer.graphicsCreated(VK_NULL_HANDLE,f.native.graphics,nullptr,other);});
   while(!f.vk.insidePipeline.load())std::this_thread::yield();
   const auto result=f.submit(false);f.vk.blockPipeline=false;cold.join();
   check(result==VK_SUCCESS,"contended native submit never waits for cold renderer ownership");
  }
  check(f.vk.reads.back()==0,"unsupported native batch contains zero guard and no prepared one guard");
  const auto fresh=f.freshFrame();
  check(fresh.ready&&fresh.coverage!=initial.coverage,"fresh coverage survives unsupported batch without recycling retained native recording");
  check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"subsequent supported fresh picture immediately restores exact coverage");
 }
}
void integration(const std::vector<std::uint32_t>& vs,const std::vector<std::uint32_t>& ps){
 {coverageReadable=false;{Integration f(vs,ps,1,true,false);check(f.publicPipeline==handle<VkPipeline>(0x60)&&f.vk.createdPipelines.empty(),"unavailable coverage artifact preserves exact ordinary native pipeline without a fallback decoder");
  const auto before=f.vk.submits;check(f.renderer.submit(f.vk.queue,0,nullptr,VK_NULL_HANDLE)==VK_SUCCESS&&f.vk.submits==before+1,"unavailable coverage forwards native submit exactly once without guard commands");
  check(!consumer.owner,"missing coverage never advertises an exact-mask route");}coverageReadable=true;}
 {Integration f(vs,ps,1);auto draw=f.record();check(draw.ready&&draw.coverage&&draw.control,"documented native 29-binding count1 gets exact-coverage companion pipeline");check(f.submit()==VK_SUCCESS&&f.vk.reads.back()==1,"native count1 current exact mask reaches original draw");}
 {Integration f(vs,ps,1,false,false);check(!f.record().ready,"pending count1 cannot be admitted by observed layout alone");}
 {auto wrong=ps;wrong.back()^=1;Integration f(vs,wrong,1,true,false);check(!f.record().ready,"mutated native fragment cannot authorize count1");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.ready&&draw.coverage&&draw.control&&draw.original==f.publicPipeline&&f.publicPipeline==handle<VkPipeline>(0x60)&&draw.replacement!=draw.original,"actual source import selects coverage without changing original public pipeline");check(f.submit()==VK_SUCCESS&&f.vk.reads.back()==1,"zero then exact one guard reaches original native CB");check(f.vk.originalFences.back()==f.vk.appFence&&f.vk.waitPreserved==1&&f.vk.signalPreserved==1,"original fence and semaphore contract forwarded");
  check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==1,"legal flags0 re-submit gets fresh immutable GPU guard without host UBO writes");leaseCurrent=false;check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"previously successful reused recording turns opaque once actual selected lease expires");
 }
 {Integration f(vs,ps,1);const auto baseline=f.publicPipeline;check(baseline==handle<VkPipeline>(0x60)&&f.vk.createdPipelines.size()==1&&f.vk.createdPipelines.front()!=baseline,"ordinary native handle unchanged; only separate coverage companion is created");f.record();f.renderer.commandUnknown(f.cb);check(!f.renderer.beforeDraw(f.cb).ready&&f.publicPipeline==baseline,"unknown recording retains ordinary native pipeline");f.renderer.trackingLost();check(!f.record().ready&&f.publicPipeline==baseline,"lost observer leaves caller bound to ordinary native pipeline");}
 {Integration f(vs,ps,1,true,false,true);check(f.publicPipeline==handle<VkPipeline>(0x60)&&!f.record().ready,"coverage creation failure preserves ordinary native pipeline");}
 {Integration f(vs,ps,1,true,false,false,true);check(f.publicPipeline==handle<VkPipeline>(0x60)&&f.vk.createdPipelines.empty(),"unsupported custom allocator leaves original native handle untouched");}
 {Integration f(vs,ps,1);const auto baseline=f.publicPipeline,coverage=f.vk.createdPipelines.back();f.renderer.pipelineDestroyed(baseline);check(std::count(f.vk.destroyedPipelines.begin(),f.vk.destroyedPipelines.end(),handle<VkPipeline>(0x60))==0&&std::count(f.vk.destroyedPipelines.begin(),f.vk.destroyedPipelines.end(),coverage)==1,"public pipeline retirement releases only private coverage once");check(std::find(f.vk.destroyedPipelines.begin(),f.vk.destroyedPipelines.end(),baseline)==f.vk.destroyedPipelines.end(),"renderer never double-destroys caller-owned native pipeline");Driver::destroyPipeline(f.device,baseline,nullptr);f.renderer.stopCold();check(std::count(f.vk.destroyedPipelines.begin(),f.vk.destroyedPipelines.end(),baseline)==1,"downstream original destroy owns native pipeline exactly once");}
 {Integration f(vs,ps);f.connection->store(false);auto draw=f.record();check(!draw.ready&&!draw.coverage&&!draw.control&&f.publicPipeline==handle<VkPipeline>(0x60),"missing compatible current picture keeps ordinary native RGB, never a legacy atlas");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record coverage before connection loss");f.connection->store(false);check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"source connection loss revokes recorded draw before submit");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record exact descriptor before legal rewrite");f.writeView(handle<VkImageView>(0xdead));check(f.submit(false)==VK_SUCCESS&&f.vk.reads.back()==0,"descriptor rewrite after record cannot authorize a different native selected image");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record before unknown command");f.renderer.commandUnknown(f.cb);f.submit(false);check(f.vk.reads.back()==0,"unknown recorded command revokes guard");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"record before reset");f.renderer.commandReset(f.cb);f.submit(false);check(f.vk.reads.back()==0,"reset drops old recorded picture and guard");}
 {Integration f(vs,ps);auto draw=f.record(VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);check(!draw.ready&&!draw.control&&!draw.coverage,"SIMULT recording stays ordinary native even with ready mask");}
 {Integration f(vs,ps);auto draw=f.record();check(draw.control!=VK_NULL_HANDLE,"descriptor initially current");f.renderer.commandReset(f.cb);f.renderer.viewDestroyed(f.view);VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=f.image;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};f.renderer.viewCreated(f.view,ci);draw=f.record();check(!draw.ready&&!draw.control,"numerically reused view cannot revive an old descriptor serial");f.writeView(f.view);draw=f.record();check(draw.control!=VK_NULL_HANDLE,"actual new descriptor write restores current view");}
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
int main(int argc,char** argv){try{check(argc==4||argc==5,"explicit private inputs required");auto vs=words(argv[1]),ps=words(argv[2]);coverageWords=words(argv[3]);
 if(argc==5&&std::strcmp(argv[4],"idle-startup")==0)idleStartup(vs,ps);
 else if(argc==5&&std::strcmp(argv[4],"telemetry")==0)telemetry(vs,ps);
 else if(argc==5&&std::strcmp(argv[4],"fallback-recovery")==0)fallbackRecovery(vs,ps);
 else if(argc==5&&std::strcmp(argv[4],"timeline")==0)timeline(vs,ps);
 else if(argc==5&&std::strcmp(argv[4],"reservation-recovery")==0)reservationRecovery(vs,ps);
 else if(argc==5&&std::strcmp(argv[4],"continuous")==0)continuous(vs,ps);
 else if(argc==5){check(std::strcmp(argv[4],"artifact-fallback")==0,"explicit bounded renderer subset");coverageReadable=false;{Integration f(vs,ps,1,true,false);check(f.publicPipeline==handle<VkPipeline>(0x60)&&f.vk.createdPipelines.empty(),"unavailable coverage artifact preserves exact ordinary native pipeline without a fallback decoder");const auto before=f.vk.submits;check(f.renderer.submit(f.vk.queue,0,nullptr,VK_NULL_HANDLE)==VK_SUCCESS&&f.vk.submits==before+1,"unavailable coverage forwards native submit exactly once without guard commands");check(!consumer.owner,"missing coverage never advertises an exact-mask route");}coverageReadable=true;{Integration f(vs,ps,1);check(f.record().coverage!=VK_NULL_HANDLE,"valid count1 artifact and exact native interface still prepare full coverage");}}
 else {idleStartup(vs,ps);integration(vs,ps);fallbackRecovery(vs,ps);telemetry(vs,ps);timeline(vs,ps);reservationRecovery(vs,ps);continuous(vs,ps);}
 std::cout<<"{\"checks\":"<<checks<<",\"result\":\"PASS\"}\n";return 0;}catch(const std::exception& e){std::cerr<<"renderer failed after "<<checks<<": "<<e.what()<<'\n';return 1;}}
