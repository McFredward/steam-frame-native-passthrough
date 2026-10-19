#include "../../layer/stream_bridge_coverage_gpu.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace stream_bridge_coverage_gpu;
namespace {
unsigned checks=0;void check(bool x,const char*m){++checks;if(!x)throw std::runtime_error(m);}
template<class T>T handle(std::uint64_t n){return (T)(std::uintptr_t)n;}
template<class T>std::uint64_t number(T h){return (std::uint64_t)(std::uintptr_t)h;}
struct Fake {
    std::uint64_t next=100;unsigned calls=0,submits=0,destroys=0,allocations=0,barriers=0;
    bool failSubmit=false,failReset=false,failImage=false,autoSignal=false;std::atomic<bool> block{false},inside{false};
    std::map<std::uint64_t,std::vector<std::uint8_t>> memory;std::map<std::uint64_t,bool> fences;std::vector<std::uint64_t> pending;
    VkQueue queue=handle<VkQueue>(3);static Fake* active;
    template<class T>T make(){++calls;return handle<T>(++next);}
    static void VKAPI_CALL getQueue(VkDevice,std::uint32_t family,std::uint32_t index,VkQueue*out){auto&f=*active;++f.calls;check(family==0&&index==0,"exact prepared queue");*out=f.queue;}
#define CREATE(Name,Type,Info) static VkResult VKAPI_CALL create##Name(VkDevice,const Info*,const VkAllocationCallbacks*,Type*out){*out=active->make<Type>();return VK_SUCCESS;}
    CREATE(DescriptorSetLayout,VkDescriptorSetLayout,VkDescriptorSetLayoutCreateInfo)
    CREATE(DescriptorPool,VkDescriptorPool,VkDescriptorPoolCreateInfo)
    CREATE(Sampler,VkSampler,VkSamplerCreateInfo)
    CREATE(CommandPool,VkCommandPool,VkCommandPoolCreateInfo)
    CREATE(ImageView,VkImageView,VkImageViewCreateInfo)
    CREATE(Buffer,VkBuffer,VkBufferCreateInfo)
#undef CREATE
    static VkResult VKAPI_CALL createImage(VkDevice,const VkImageCreateInfo*i,const VkAllocationCallbacks*,VkImage*out){auto&f=*active;++f.calls;check(i->format==VK_FORMAT_R8_UNORM&&i->usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),"immutable R8 image contract");if(f.failImage)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*out=f.make<VkImage>();return VK_SUCCESS;}
#define DESTROY(Name,Type) static void VKAPI_CALL destroy##Name(VkDevice,Type,const VkAllocationCallbacks*){++active->calls;++active->destroys;}
    DESTROY(DescriptorSetLayout,VkDescriptorSetLayout) DESTROY(DescriptorPool,VkDescriptorPool) DESTROY(Sampler,VkSampler)
    DESTROY(CommandPool,VkCommandPool) DESTROY(ImageView,VkImageView) DESTROY(Image,VkImage) DESTROY(Buffer,VkBuffer)
#undef DESTROY
    static VkResult VKAPI_CALL allocateSets(VkDevice,const VkDescriptorSetAllocateInfo*i,VkDescriptorSet*out){for(unsigned n=0;n<i->descriptorSetCount;++n)out[n]=active->make<VkDescriptorSet>();return VK_SUCCESS;}
    static void VKAPI_CALL update(VkDevice,std::uint32_t n,const VkWriteDescriptorSet*w,std::uint32_t,const VkCopyDescriptorSet*){++active->calls;check(n==1&&w->dstBinding==0&&w->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,"set1binding0 sampled mask");}
    static VkResult VKAPI_CALL allocateCommands(VkDevice,const VkCommandBufferAllocateInfo*i,VkCommandBuffer*out){for(unsigned n=0;n<i->commandBufferCount;++n)out[n]=active->make<VkCommandBuffer>();return VK_SUCCESS;}
    static VkResult VKAPI_CALL resetCommand(VkCommandBuffer,VkCommandBufferResetFlags){++active->calls;return VK_SUCCESS;}
    static VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*i){++active->calls;check(i->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,"copy one-time only");return VK_SUCCESS;}
    static VkResult VKAPI_CALL end(VkCommandBuffer){++active->calls;return VK_SUCCESS;}
    static void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t n,const VkImageMemoryBarrier*b){auto&f=*active;++f.calls;++f.barriers;check(n==1&&b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED,"no ownership guess");if(b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)check(src==VK_PIPELINE_STAGE_TRANSFER_BIT&&dst==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&b->dstAccessMask==VK_ACCESS_SHADER_READ_BIT,"copy visibility to native fragment");}
    static void VKAPI_CALL copy(VkCommandBuffer,VkBuffer,VkImage,VkImageLayout l,std::uint32_t n,const VkBufferImageCopy*c){++active->calls;check(l==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&n==1&&c->imageExtent.depth==1,"copy extent");}
    static VkResult VKAPI_CALL createFence(VkDevice,const VkFenceCreateInfo*i,const VkAllocationCallbacks*,VkFence*out){check(i->flags==0,"unsignaled owned fence");*out=active->make<VkFence>();active->fences[number(*out)]=false;return VK_SUCCESS;}
    static void VKAPI_CALL destroyFence(VkDevice,VkFence f,const VkAllocationCallbacks*){++active->calls;++active->destroys;active->fences.erase(number(f));}
    static VkResult VKAPI_CALL resetFences(VkDevice,std::uint32_t n,const VkFence*fs){++active->calls;if(active->failReset)return VK_ERROR_DEVICE_LOST;for(unsigned i=0;i<n;++i)active->fences[number(fs[i])]=false;return VK_SUCCESS;}
    static VkResult VKAPI_CALL fenceStatus(VkDevice,VkFence f){++active->calls;return active->fences.at(number(f))?VK_SUCCESS:VK_NOT_READY;}
    static void VKAPI_CALL imageReq(VkDevice,VkImage,VkMemoryRequirements*out){++active->calls;*out={16384,1,1};}
    static void VKAPI_CALL bufferReq(VkDevice,VkBuffer,VkMemoryRequirements*out){++active->calls;*out={16384,1,1};}
    static VkResult VKAPI_CALL bindImage(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize){++active->calls;return VK_SUCCESS;}
    static VkResult VKAPI_CALL bindBuffer(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize){++active->calls;return VK_SUCCESS;}
    static VkResult VKAPI_CALL allocate(VkDevice,const VkMemoryAllocateInfo*i,const VkAllocationCallbacks*,VkDeviceMemory*out){*out=active->make<VkDeviceMemory>();++active->allocations;active->memory[number(*out)].resize(i->allocationSize);return VK_SUCCESS;}
    static void VKAPI_CALL freeMemory(VkDevice,VkDeviceMemory m,const VkAllocationCallbacks*){++active->calls;++active->destroys;active->memory.erase(number(m));}
    static VkResult VKAPI_CALL map(VkDevice,VkDeviceMemory m,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void**out){++active->calls;*out=active->memory.at(number(m)).data();return VK_SUCCESS;}
    static void VKAPI_CALL unmap(VkDevice,VkDeviceMemory){++active->calls;}
    static VkResult VKAPI_CALL flush(VkDevice,std::uint32_t n,const VkMappedMemoryRange*r){++active->calls;check(n==1&&r->offset==0&&r->size==VK_WHOLE_SIZE,"noncoherent range aligned by whole allocation");return VK_SUCCESS;}
    static VkResult VKAPI_CALL submit(VkQueue q,std::uint32_t n,const VkSubmitInfo*s,VkFence f){auto&a=*active;++a.calls;++a.submits;check(q==a.queue,"queue owner exact");check((n==0&&!s)||(n==1&&s&&s->commandBufferCount==1),"copy or empty retirement submission");a.inside=true;while(a.block.load())std::this_thread::yield();a.inside=false;if(a.failSubmit)return VK_ERROR_DEVICE_LOST;if(a.autoSignal)a.fences.at(number(f))=true;else a.pending.push_back(number(f));return VK_SUCCESS;}
    static PFN_vkVoidFunction VKAPI_CALL get(VkDevice,const char*n){
#define FN(Name,Body) if(std::strcmp(n,"vk" #Name)==0)return reinterpret_cast<PFN_vkVoidFunction>(&Body);
        FN(GetDeviceQueue,getQueue) FN(CreateDescriptorSetLayout,createDescriptorSetLayout) FN(DestroyDescriptorSetLayout,destroyDescriptorSetLayout)
        FN(CreateDescriptorPool,createDescriptorPool) FN(DestroyDescriptorPool,destroyDescriptorPool) FN(AllocateDescriptorSets,allocateSets) FN(UpdateDescriptorSets,update)
        FN(CreateSampler,createSampler) FN(DestroySampler,destroySampler) FN(CreateCommandPool,createCommandPool) FN(DestroyCommandPool,destroyCommandPool)
        FN(AllocateCommandBuffers,allocateCommands) FN(ResetCommandBuffer,resetCommand) FN(BeginCommandBuffer,begin) FN(EndCommandBuffer,end)
        FN(CmdPipelineBarrier,barrier) FN(CmdCopyBufferToImage,copy) FN(CreateFence,createFence) FN(DestroyFence,destroyFence) FN(ResetFences,resetFences) FN(GetFenceStatus,fenceStatus)
        FN(CreateImage,createImage) FN(DestroyImage,destroyImage) FN(GetImageMemoryRequirements,imageReq) FN(BindImageMemory,bindImage)
        FN(CreateImageView,createImageView) FN(DestroyImageView,destroyImageView) FN(CreateBuffer,createBuffer) FN(DestroyBuffer,destroyBuffer) FN(GetBufferMemoryRequirements,bufferReq)
        FN(BindBufferMemory,bindBuffer) FN(AllocateMemory,allocate) FN(FreeMemory,freeMemory) FN(MapMemory,map) FN(UnmapMemory,unmap) FN(FlushMappedMemoryRanges,flush) FN(QueueSubmit,submit)
#undef FN
        return nullptr;
    }
    void signal(){for(auto x:pending)fences.at(x)=true;pending.clear();}
};Fake* Fake::active=nullptr;
struct Fixture {
    Fake vk;Pool pool;std::array<std::uint8_t,16> pixels{{0,16,32,64,128,192,240,255}};
    Fixture(bool coherent=true){Fake::active=&vk;VkPhysicalDeviceMemoryProperties m{};m.memoryTypeCount=1;m.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|(coherent?VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:0);
        check(pool.prepare(handle<VkDevice>(2),Fake::get,m,0),"actual Pool prepare");check(pool.descriptorLayout()!=VK_NULL_HANDLE,"owned descriptor layout");}
    ~Fixture(){pool.destroyAfterIdle();}
    FrameIdentity frame(std::uint64_t id){FrameIdentity f{};f.binding[0]=1;f.descriptor={id,1,16,1};return f;}
    bool upload(std::uint64_t id){return pool.upload(frame(id),16,1,{pixels.data(),pixels.size()});}
    Lease ready(std::uint64_t id){check(upload(id),"worker upload");pool.drainUploads(vk.queue);vk.signal();pool.reapCold();auto l=pool.acquire(frame(id));check(bool(l),"completed upload ready");return l;}
};
void lifecycle(){Fixture f(false);auto id=f.frame(1);check(f.upload(1),"upload valid");check(!f.pool.acquire(id),"pending is never sampled");auto before=f.vk.submits;f.pool.drainUploads(handle<VkQueue>(999));check(f.vk.submits==before,"unknown queue untouched");f.pool.drainUploads(f.vk.queue);check(!f.pool.acquire(id),"unsignaled copy never sampled");f.pool.reapCold();check(!f.pool.acquire(id),"poll does not invent completion");f.vk.signal();f.pool.reapCold();unsigned calls=f.vk.calls;auto l=f.pool.acquire(id);check(bool(l),"upload fence completion permits binding");check(f.vk.calls==calls,"draw acquires without Vulkan call");const auto reservation=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(reservation),"reserve retirement");f.pool.submitted(reservation,f.vk.queue,&l,1,VK_SUCCESS);f.pool.releaseRecorded(l);check(f.pool.statusCold().submitted==1,"record release does not free in-flight GPU reference");f.pool.reapCold();check(f.pool.statusCold().submitted==1,"tail fence required");f.vk.signal();f.pool.reapCold();check(f.pool.statusCold().submitted==0,"tail fence releases GPU ownership");}
void inputs(){Fixture f;auto id=f.frame(1);auto invalid=id;invalid.binding={};check(!f.pool.upload(invalid,16,1,{f.pixels.data(),16}),"no empty binding");invalid=id;invalid.descriptor.sourceFrame=0;check(!f.pool.upload(invalid,16,1,{f.pixels.data(),16}),"no guessed frame0");invalid=id;invalid.descriptor.encoderGeneration=0;check(!f.pool.upload(invalid,16,1,{f.pixels.data(),16}),"no generation0");check(!f.pool.upload(id,8,2,{f.pixels.data(),16}),"extent must match authenticated descriptor");check(!f.pool.upload(id,16,1,{f.pixels.data(),15}),"exact bytes required");check(f.upload(1)&&f.upload(1),"immutable duplicate accepted");f.pixels[0]=17;check(!f.upload(1),"conflicting same-frame bytes refused");}
void bounded(){Fixture f;std::array<Lease,MaximumSlots> ls{};for(unsigned i=0;i<MaximumSlots;++i)ls[i]=f.ready(i+1);check(!f.upload(9),"eight recorded slots cannot be evicted");const auto allocations=f.vk.allocations,destroys=f.vk.destroys;f.pool.releaseRecorded(ls[0]);check(f.upload(9),"unreferenced ready slot recycled");check(f.vk.allocations==allocations&&f.vk.destroys==destroys,"same extent reuses idle image and staging allocations");f.pool.drainUploads(f.vk.queue);f.vk.signal();f.pool.reapCold();auto l=f.pool.acquire(f.frame(9));check(bool(l)&&l.slot==ls[0].slot&&l.generation!=ls[0].generation,"reused slot has fresh generation");auto n=f.pool.statusCold().recorded;f.pool.releaseRecorded(ls[0]);check(f.pool.statusCold().recorded==n,"stale release cannot decrement new slot");f.pool.releaseRecorded(l);for(unsigned i=1;i<MaximumSlots;++i)f.pool.releaseRecorded(ls[i]);}
void failure(){
    {Fixture f;f.vk.failImage=true;check(!f.upload(1),"allocation failure refused");check(!f.pool.acquire(f.frame(1)),"failed upload cannot bind");f.vk.failImage=false;check(f.upload(2),"failed allocation releases slot");}
    {Fixture f;check(f.upload(1),"pending before queue failure");f.vk.failSubmit=true;f.pool.drainUploads(f.vk.queue);check(!f.pool.acquire(f.frame(1))&&f.pool.statusCold().quarantined==1,"failed copy quarantines");}
    {Fixture f;auto l=f.ready(1);const auto reservation=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(reservation),"tail reservation");f.vk.failSubmit=true;f.pool.submitted(reservation,f.vk.queue,&l,1,VK_SUCCESS);f.pool.releaseRecorded(l);check(f.pool.statusCold().quarantined==1&&f.pool.statusCold().submitted==1,"tail failure retains resources");check(!f.pool.acquire(f.frame(1)),"quarantined source not reused");}
    {Fixture f;auto l=f.ready(1);const auto reservation=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(reservation),"original-error reservation");auto submits=f.vk.submits;f.pool.submitted(reservation,f.vk.queue,&l,1,VK_ERROR_DEVICE_LOST);check(f.vk.submits==submits&&f.pool.statusCold().quarantined==1,"original error not rewritten or submitted again");f.pool.releaseRecorded(l);}
    {Fixture f;auto l=f.ready(1);for(unsigned i=0;i<MaximumSubmissionFences;++i){const auto reservation=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(reservation),"bounded retirement reservation");f.pool.submitted(reservation,f.vk.queue,&l,1,VK_SUCCESS);}check(!f.pool.prepareSubmit(f.vk.queue,&l,1),"retirement capacity refuses and retains");check(f.pool.statusCold().quarantined==0&&f.pool.statusCold().submitted==MaximumSubmissionFences,"capacity refusal retains actual in-flight ownership without poisoning untouched batches");f.vk.signal();f.pool.reapCold();const auto retried=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(retried),"reservation recovers immediately after actual retirement");f.pool.submitted(retried,f.vk.queue,&l,1,VK_SUCCESS);f.pool.releaseRecorded(l);}
}
void scheduled(){
    auto prime=[](Fixture&f){for(unsigned i=0;i<MaximumSlots;++i){auto l=f.ready(i+1);f.pool.releaseRecorded(l);}};
    {Fixture f;check(f.upload(1),"first scheduled upload");check(!f.pool.acquireScheduled(f.frame(1)),"scheduled never exposes first uninitialized image");
        f.pool.drainUploads(f.vk.queue);check(!f.pool.acquireScheduled(f.frame(1)),"scheduled first copy still requires completed upload fence");
        f.vk.signal();f.pool.reapCold();auto l=f.pool.acquireScheduled(f.frame(1));check(bool(l),"scheduled first image allowed after real reap");f.pool.releaseRecorded(l);}
    {Fixture f;prime(f);auto allocations=f.vk.allocations;check(f.upload(9),"reuse pending mask");check(f.vk.allocations==allocations,"same extent reuses image and staging allocations");
        check(!f.pool.acquire(f.frame(9)),"strict acquire remains ready-only");auto calls=f.vk.calls;auto l=f.pool.acquireScheduled(f.frame(9));check(bool(l),"scheduled initialized reuse is recordable before copy submit");check(f.vk.calls==calls,"scheduled acquire has no Vulkan calls");
        check(!f.pool.prepareSubmit(f.vk.queue,&l,1),"scheduled pending image cannot submit before actual copy acceptance");check(f.pool.statusCold().quarantined==0,"unsubmitted scheduled reference retains recording storage without permanent quarantine");f.pool.drainUploads(f.vk.queue);const auto retried=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(retried),"same scheduled reference recovers only after actual copy acceptance");f.pool.submitted(retried,f.vk.queue,&l,1,VK_SUCCESS);f.pool.releaseRecorded(l);}
    {Fixture f;prime(f);check(f.upload(9),"accepted scheduled upload");auto l=f.pool.acquireScheduled(f.frame(9));check(bool(l),"scheduled valid reused descriptor");
        f.pool.drainUploads(f.vk.queue);check(!f.pool.acquire(f.frame(9)),"strict acquire does not inherit ordered queue authority");
        const auto reservation=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(reservation),"copy accepted on exact queue permits scheduled mask before reap");f.pool.submitted(reservation,f.vk.queue,&l,1,VK_SUCCESS);f.pool.releaseRecorded(l);
        check(f.pool.statusCold().copying==1&&f.pool.statusCold().submitted==1,"unreaped upload and GPU ownership retained");f.vk.signal();f.pool.reapCold();
        check(f.pool.statusCold().submitted==0,"scheduled caller retirement completed");auto strict=f.pool.acquire(f.frame(9));check(bool(strict),"strict acquire enabled by actual copy reap");f.pool.releaseRecorded(strict);}
    {Fixture f;prime(f);check(f.upload(9),"failed-copy reused upload");auto l=f.pool.acquireScheduled(f.frame(9));auto destroys=f.vk.destroys;
        f.vk.failSubmit=true;f.pool.drainUploads(f.vk.queue);check(!f.pool.prepareSubmit(f.vk.queue,&l,1),"failed copy never authorizes scheduled submission");
        check(f.vk.destroys==destroys&&f.pool.statusCold().quarantined==1,"failed copy retains previously initialized descriptor and resources");f.pool.releaseRecorded(l);}
    {Fixture f;prime(f);auto resized=f.frame(9);resized.descriptor.codedWidth=8;resized.descriptor.codedHeight=2;
        check(f.pool.upload(resized,8,2,{f.pixels.data(),f.pixels.size()}),"new extent upload");check(!f.pool.acquireScheduled(resized),"new extent cannot reuse initialized-image authority");
        f.pool.drainUploads(f.vk.queue);check(!f.pool.acquireScheduled(resized),"new extent remains uninitialized until actual reap");f.vk.signal();f.pool.reapCold();auto l=f.pool.acquireScheduled(resized);check(bool(l),"new extent allowed after actual copy completion");f.pool.releaseRecorded(l);}
}
void reservations(){
    {Fixture f;auto l=f.ready(1);const auto old=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(old),"first exact reservation");f.pool.submitted(old,f.vk.queue,&l,1,VK_SUCCESS);f.vk.signal();f.pool.reapCold();
        const auto fresh=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(fresh),"same fixed ticket slot reused after actual fence reset");auto before=f.vk.submits;
        f.pool.submitted(old,f.vk.queue,&l,1,VK_SUCCESS);check(f.vk.submits==before&&f.pool.statusCold().submitted==1&&!f.pool.statusCold().quarantined,"stale reservation cannot claim a newer ticket generation");
        f.pool.submitted(Reservation{},f.vk.queue,&l,1,VK_SUCCESS);check(f.vk.submits==before&&!f.pool.statusCold().quarantined,"default token cannot consume current reservation");
        f.pool.submitted(fresh,f.vk.queue,&l,1,VK_SUCCESS);check(f.vk.submits==before+1,"exact fresh reservation submits its one original tail fence");
        f.pool.submitted(fresh,f.vk.queue,&l,1,VK_SUCCESS);check(f.vk.submits==before+1&&!f.pool.statusCold().quarantined,"duplicate completion never submits a second tail or damages current storage");
        f.vk.signal();f.pool.reapCold();check(f.pool.statusCold().submitted==0,"accepted reservation retires through real completion");f.pool.releaseRecorded(l);}
    Reservation foreign;
    {Fixture f;auto l=f.ready(1);foreign=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(foreign),"foreign owner produces a legitimate reservation");f.pool.submitted(foreign,f.vk.queue,&l,1,VK_SUCCESS);f.vk.signal();f.pool.reapCold();f.pool.releaseRecorded(l);}
    {Fixture f;auto l=f.ready(1);const auto own=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(own),"new pool has its own same ordinal reservation");auto before=f.vk.submits;f.pool.submitted(foreign,f.vk.queue,&l,1,VK_SUCCESS);
        check(f.vk.submits==before&&f.pool.statusCold().submitted==1&&!f.pool.statusCold().quarantined,"foreign pool token cannot consume same slot and generation");f.pool.submitted(own,f.vk.queue,&l,1,VK_SUCCESS);check(f.vk.submits==before+1,"foreign-token refusal leaves exact own completion usable");f.vk.signal();f.pool.reapCold();f.pool.releaseRecorded(l);}
    {Fixture f;auto l=f.ready(1);const auto r=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(r),"mismatch case has a real reservation");auto wrong=l;++wrong.generation;auto before=f.vk.submits;f.pool.submitted(r,f.vk.queue,&wrong,1,VK_SUCCESS);
        check(f.vk.submits==before&&f.pool.statusCold().submitted==1&&f.pool.statusCold().quarantined==1,"post-submit lease mismatch keeps actual in-flight ownership quarantined");f.pool.releaseRecorded(l);}
    {Fixture f;auto l=f.ready(1);const auto old=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(old),"old device lifecycle has a reservation");f.pool.submitted(old,f.vk.queue,&l,1,VK_SUCCESS);f.vk.signal();f.pool.reapCold();f.pool.releaseRecorded(l);f.pool.destroyAfterIdle();
        VkPhysicalDeviceMemoryProperties memory{};memory.memoryTypeCount=1;memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        check(f.pool.prepare(handle<VkDevice>(2),Fake::get,memory,0),"same pool re-prepared only after actual idle destruction");l=f.ready(1);const auto fresh=f.pool.prepareSubmit(f.vk.queue,&l,1);check(bool(fresh),"new device lifecycle has its fresh exact reservation");auto before=f.vk.submits;f.pool.submitted(old,f.vk.queue,&l,1,VK_SUCCESS);
        check(f.vk.submits==before&&f.pool.statusCold().submitted==1&&!f.pool.statusCold().quarantined,"completed previous lifecycle token cannot claim re-prepared pool");f.pool.submitted(fresh,f.vk.queue,&l,1,VK_SUCCESS);check(f.vk.submits==before+1,"fresh lifecycle token remains usable after old token refusal");f.vk.signal();f.pool.reapCold();f.pool.releaseRecorded(l);}
    {Fixture f;check(!f.pool.prepareSubmit(f.vk.queue,nullptr,0)&&!f.pool.statusCold().quarantined,"ordinary empty batch needs no sampling reservation or quarantine");}
}
void warmSlots(Fixture& f){for(unsigned n=1;n<=MaximumSlots;++n){auto lease=f.ready(n);f.pool.releaseRecorded(lease);}}
void gpuReferencedReuse(){Fixture f;std::array<Lease,MaximumSlots> leases{};for(unsigned n=0;n<MaximumSlots;++n)leases[n]=f.ready(n+1);
    for(const auto& lease:leases){const auto reservation=f.pool.prepareSubmit(f.vk.queue,&lease,1);check(bool(reservation),"actual in-flight GPU reservation owns its exact source");f.pool.submitted(reservation,f.vk.queue,&lease,1,VK_SUCCESS);f.pool.releaseRecorded(lease);}
    const auto allocations=f.vk.allocations,destroys=f.vk.destroys;check(f.pool.statusCold().submitted==MaximumSlots&&!f.pool.statusCold().recorded,"all exact sources remain GPU-owned after recording references release");
    check(!f.upload(9)&&f.vk.allocations==allocations&&f.vk.destroys==destroys,"post-claim GPU ownership prevents any cold resource replacement");
    f.vk.signal();f.pool.reapCold();check(f.upload(9),"only real GPU retirement permits cold replacement");
}
void submitting(){Fixture f;warmSlots(f);check(f.upload(9),"initialized next source is pending");f.vk.block=true;std::thread queue([&]{f.pool.drainUploads(f.vk.queue);});while(!f.vk.inside.load())std::this_thread::yield();
    // The native copy submit has not returned. Its slot is not idle, even
    // though no recording or GPU sampling reference owns it yet.
    const auto calls=f.vk.calls;f.pool.reapCold();check(f.vk.calls==calls,"cold reap cannot poll a Submitting copy fence before native success");check(f.upload(9)&&f.vk.calls==calls,"duplicate certificate cannot reset or rewrite a Submitting copy");
    auto preview=f.pool.acquireScheduled(f.frame(9));check(bool(preview)&&!f.pool.acquire(f.frame(9)),"initialized Submitting image permits recording preview but no completed-upload claim");f.pool.releaseRecorded(preview);
    for(unsigned n=10;n<17;++n)check(f.upload(n),"other idle slots can receive independent exact masks while copy submit is blocked");
    const bool refused=!f.upload(17);f.vk.block=false;queue.join();check(refused,"Submitting slot cannot be recycled even after all seven other idle slots are consumed");
    f.vk.signal();f.pool.reapCold();auto lease=f.pool.acquire(f.frame(9));check(bool(lease)&&f.pool.statusCold().ready==8,"original Submitting source survives through real success and completion");f.pool.releaseRecorded(lease);
}
void concurrentReuse(){Fixture f;warmSlots(f);std::atomic<std::uint64_t> published{8};std::atomic<bool> start{false},done{false},invalid{false};std::atomic<unsigned> held{0};
    std::thread cold([&]{while(!start.load())std::this_thread::yield();for(std::uint64_t id=9;id<=4096;++id){if(!f.upload(id)){invalid=true;break;}f.pool.drainUploads(f.vk.queue);f.vk.signal();f.pool.reapCold();published.store(id,std::memory_order_release);}done=true;});
    start=true;
    while(!done.load(std::memory_order_acquire)){const auto id=published.load(std::memory_order_acquire);auto first=f.pool.acquire(f.frame(id));if(!first)continue;
        ++held;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!done.load(std::memory_order_acquire)&&published.load(std::memory_order_acquire)<id+16&&std::chrono::steady_clock::now()<until)std::this_thread::yield();
        auto second=f.pool.acquire(f.frame(id));if(!second||second.slot!=first.slot||second.generation!=first.generation||second.set!=first.set)invalid=true;
        if(second)f.pool.releaseRecorded(second);
        f.pool.releaseRecorded(first);
    }
    cold.join();check(!invalid&&held.load()>0,"concurrent cold slot reuse never changes metadata protected by a hot reference claim");check(!f.pool.statusCold().quarantined,"concurrent ref/writer claims retain bounded storage without quarantine");
}
void nonblocking(){Fixture f;check(f.upload(1),"pending for contention");f.vk.block=true;std::thread queue([&]{f.pool.drainUploads(f.vk.queue);});while(!f.vk.inside.load())std::this_thread::yield();auto start=std::chrono::steady_clock::now();check(!f.pool.acquire(f.frame(1)),"busy queue state refuses draw");check(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(50),"draw never waits for queue owner");f.vk.block=false;queue.join();}
}
int main(){try{lifecycle();inputs();bounded();failure();scheduled();reservations();gpuReferencedReuse();submitting();concurrentReuse();nonblocking();std::cout<<"{\"checks\":"<<checks<<",\"result\":\"PASS\"}\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
