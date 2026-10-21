#include "stream_bridge_renderer.h"
#include "stream_bridge_report_policy.h"
#include "stream_bridge_process.h"
#include "stream_bridge_coverage_gpu.h"
#include "native_presenter_pipeline.h"
#include "pixel_ssbo_profile_transform.h"
#include "submit_timeline_contract.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
namespace gpu=stream_bridge_coverage_gpu;
namespace sb=stream_bridge;
namespace native=stream_bridge_native;
namespace local=stream_bridge_local;
namespace journal=frame_pixel_ssbo_journal;
namespace stream_bridge_renderer {
// Implemented by the isolated camera-client owner; this query is atomic only.
bool cameraAlphaReady() noexcept;
std::uint64_t cameraAlphaEpoch() noexcept;
namespace {
template<class T>std::uint64_t handle(T value){std::uint64_t n=0;static_assert(sizeof value<=sizeof n,"bounded native handle");std::memcpy(&n,&value,sizeof value);return n;}
template<class T>T from(std::uint64_t value){T result{};std::memcpy(&result,&value,sizeof result);return result;}
template<class T,std::size_t N>T* find(std::array<T,N>& list,std::uint64_t value){for(auto& entry:list)if(entry.id==value)return &entry;return nullptr;}
template<class T,std::size_t N>T* add(std::array<T,N>& list,std::uint64_t value){if(!value||find(list,value))return nullptr;for(auto& entry:list)if(!entry.id){entry.id=value;return &entry;}return nullptr;}
bool current(const LocalPicture& picture){return picture.leases&&picture.connection&&picture.connection->load(std::memory_order_acquire)&&picture.leases->current(picture.metadata.lease);}
bool same(const sb::FrameDescriptor&a,const sb::FrameDescriptor&b){return a.sourceFrame==b.sourceFrame&&a.encoderGeneration==b.encoderGeneration&&a.codedWidth==b.codedWidth&&a.codedHeight==b.codedHeight;}
void coldNote(const char* message) noexcept{
    // Cold creation callbacks only. Repeated unrelated startup pipelines must
    // not consume the later shader/companion admission messages' entire budget.
    try{static std::mutex gate;static std::array<const char*,32> seen{};
        std::lock_guard<std::mutex> lock(gate);
        for(const auto* old:seen)if(old&&!std::strcmp(old,message))return;
        for(auto& old:seen)if(!old){old=message;std::fprintf(stderr,"[Steam Link Native Passthrough] %s\n",message);return;}
    }catch(...){/* Optional cold diagnostics cannot affect native creation. */}
}
}
struct Renderer::Impl {
    struct Memory{std::uint64_t id=0,generation=0,device=0,inode=0;int duplicate=-1;};
    struct Image{std::uint64_t id=0,generation=0,memory=0,memoryGeneration=0;std::uint32_t width=0,height=0;};
    struct View{std::uint64_t id=0,image=0,generation=0,serial=0;};
    struct Descriptor{std::uint64_t id=0,view=0,viewSerial=0;bool valid=false;};
    struct DescriptorAllocation{std::uint64_t id=0,pool=0,generation=0;};
    struct Pipeline{std::uint64_t id=0;VkPipeline coverage{};VkPipelineLayout layout{};};
    struct Use{gpu::Lease lease{};LocalPicture picture;std::uint64_t descriptor=0,descriptorGeneration=0,viewSerial=0,image=0,imageGeneration=0,memoryGeneration=0;};
    struct Retention {
        gpu::Pool* pool=nullptr;gpu::Lease lease{};
        Retention(gpu::Pool& p,gpu::Lease l):pool(&p),lease(l){}
        ~Retention(){if(pool)pool->releaseRecorded(lease);}
    };
    struct PreparedPicture:LocalPicture {std::shared_ptr<Retention> retained;};
    struct Control{VkBuffer buffer{};VkDeviceSize offset=0;VkDescriptorSet set{};VkCommandBuffer one{};bool assigned=false;};
    struct Command{
        std::uint64_t id=0,pipeline=0,set0=0,epoch=0,cameraEpoch=0;bool primary=false,eligible=false;
        Control* control=nullptr;
        std::array<Use,gpu::MaximumSlots> uses{};std::size_t used=0;
    };
    std::atomic<std::uint64_t> observationEpoch{1};std::atomic<bool> lost{false};std::atomic<unsigned> admittedPipelines{0};
    // Monotonic for this native device lifetime. Before any masked recording
    // can exist, native startup submits need no owned GPU commands at all.
    // Never clear on source loss/reset: a flags=0 CB may still retain coverage.
    std::atomic<bool> guardedRecordingPossible{false},uploadsPending{false};
    mutable std::mutex gate;VkDevice device{};PFN_vkGetDeviceProcAddr gdpa{};VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t api=0,family=UINT32_MAX;std::uint64_t generation=0;bool prepared=false,coveragePrepared=false,registered=false,count1Authorized=false;
    gpu::Pool pool;journal::Journal definitions{1,{},journal::PendingCount1::RetainFacts};
    std::vector<std::uint32_t> coverageWords;
    VkDescriptorSetLayout controlLayout{};VkDescriptorPool controlPool{};VkCommandPool guardPool{};
    VkCommandBuffer zero{};VkQueue queue{};VkBuffer controlsBuffer{};VkDeviceMemory controlsMemory{};std::array<Control,128> controls{};
    // Fixed cold metadata budgets; never evict a still-live native generation.
    std::array<Memory,512> memories{};std::array<Image,512> images{};std::array<View,1024> views{};std::array<Descriptor,256> descriptors{};
    std::array<DescriptorAllocation,4096> descriptorAllocations{};std::uint64_t descriptorGeneration=0;
    std::array<Pipeline,128> pipelines{};std::array<Command,128> commands{};std::array<std::shared_ptr<const PreparedPicture>,native::MaxDevices*native::MaxBuffers> pictures{};

#define FUNCS(X) X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(DeviceWaitIdle) \
 X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdUpdateBuffer) X(CmdFillBuffer) X(QueueSubmit)
#define FIELD(n) PFN_vk##n n=nullptr;
    FUNCS(FIELD)
#undef FIELD
    PFN_vkQueueSubmit2 QueueSubmit2=nullptr,QueueSubmit2KHR=nullptr;
    std::array<gpu::Lease,gpu::MaximumSlots> submittedLeases{};std::size_t submittedCount=0;
    struct Counters {
        std::atomic<std::uint64_t> metadata{0},maskNone{0},maskDecoded{0},maskRejected{0},uploadAccepted{0},uploadRefused{0},pictures{0};
        std::atomic<std::uint64_t> maskOpaque{0},maskTransparent{0},maskMixed{0};
        std::atomic<std::uint64_t> begins{0},draws{0},selected{0},guardAttempts{0},guardAccepted{0},guardRefused{0},opaqueSubmits{0};
        std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(DrawRefusal::Count)> refusals{};
        std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(CollectRefusal::Count)> collectRefusals{};
        std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(SubmitRefusal::Count)> submitRefusals{};
        std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(DescriptorRefusal::Count)> descriptorRefusals{};
        std::atomic<std::uint64_t> imageCapacityRefused{0},memoryCapacityRefused{0},viewCapacityRefused{0},descriptorCapacityRefused{0};
        std::atomic<std::uint64_t> descriptorUntrackedWrites{0},descriptorUntrackedCopies{0};
        std::atomic<std::uint64_t> collects{0},collected{0},coreSubmits{0},submits2{0};
        std::atomic<std::uint64_t> descriptorAllocated{0},descriptorAllocationRefused{0},descriptorPoolRetirements{0},descriptorSetsRetired{0},descriptorUnknownUpdates{0},descriptorCopies{0};
        std::atomic<std::uint32_t> firstNextType{0},firstRefusedNextType{0};
    } counts;
    ColdReportPolicy reportPolicy;
    void refused(DrawRefusal reason) noexcept {counts.refusals[static_cast<std::size_t>(reason)].fetch_add(1,std::memory_order_relaxed);}
    void submitRefused(SubmitRefusal reason,std::uint32_t next=0) noexcept {
        ++counts.submitRefusals[static_cast<std::size_t>(reason)];
        std::uint32_t empty=0;if(next)counts.firstRefusedNextType.compare_exchange_strong(empty,next,std::memory_order_relaxed);
    }
    void nextObserved(std::uint32_t next) noexcept {std::uint32_t empty=0;if(next)counts.firstNextType.compare_exchange_strong(empty,next,std::memory_order_relaxed);}
    RendererStats stats()const noexcept{
        RendererStats out;
#define COUNT(n) out.n=counts.n.load(std::memory_order_relaxed);
        COUNT(metadata) COUNT(maskNone) COUNT(maskDecoded) COUNT(maskRejected) COUNT(uploadAccepted) COUNT(uploadRefused) COUNT(pictures)
        COUNT(maskOpaque) COUNT(maskTransparent) COUNT(maskMixed)
        COUNT(begins) COUNT(draws) COUNT(selected) COUNT(guardAttempts) COUNT(guardAccepted) COUNT(guardRefused) COUNT(opaqueSubmits)
        COUNT(collects) COUNT(collected) COUNT(coreSubmits) COUNT(submits2) COUNT(firstNextType) COUNT(firstRefusedNextType)
        COUNT(imageCapacityRefused) COUNT(memoryCapacityRefused) COUNT(viewCapacityRefused) COUNT(descriptorCapacityRefused)
        COUNT(descriptorUntrackedWrites) COUNT(descriptorUntrackedCopies)
        COUNT(descriptorAllocated) COUNT(descriptorAllocationRefused) COUNT(descriptorPoolRetirements) COUNT(descriptorSetsRetired) COUNT(descriptorUnknownUpdates) COUNT(descriptorCopies)
#undef COUNT
        for(std::size_t n=0;n<out.drawRefusals.size();++n)out.drawRefusals[n]=counts.refusals[n].load(std::memory_order_relaxed);
        for(std::size_t n=0;n<out.collectRefusals.size();++n)out.collectRefusals[n]=counts.collectRefusals[n].load(std::memory_order_relaxed);
        for(std::size_t n=0;n<out.submitRefusals.size();++n)out.submitRefusals[n]=counts.submitRefusals[n].load(std::memory_order_relaxed);
        for(std::size_t n=0;n<out.descriptorRefusals.size();++n)out.descriptorRefusals[n]=counts.descriptorRefusals[n].load(std::memory_order_relaxed);
        const auto gpu=pool.statusCold();out.pending=gpu.pending;out.copying=gpu.copying;out.ready=gpu.ready;out.quarantined=gpu.quarantined;
        out.poolUploaded=gpu.uploaded;out.poolRefused=gpu.refused;out.pipelines=admittedPipelines.load(std::memory_order_acquire);
        out.prepared=out.pipelines!=0;out.lost=lost.load(std::memory_order_acquire);return out;
    }
    void reportCold() noexcept{
        const auto now=std::chrono::steady_clock::now();
        RendererStats s;
        if(!reportPolicy.inspect(now,[this]{return stats();},s))return;
        const auto miss=[&](DrawRefusal r){return static_cast<unsigned long long>(s.drawRefusals[static_cast<std::size_t>(r)]);};
        std::fprintf(stderr,"[Steam Link Native Passthrough] renderer flow: metadata=%llu; mask decoded/none/rejected=%llu/%llu/%llu; upload accepted/refused=%llu/%llu, actual=%llu; pool pending/copying/ready/quarantined=%u/%u/%u/%u; pictures=%llu; record selected/attempts=%llu/%llu, begins=%llu; miss unavailable/busy/cb/epoch/pipeline/ineligible/descriptor/image/exact/mask/capacity=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; submit guarded-accepted/attempts/refused=%llu/%llu/%llu, opaque=%llu; pipelines=%u, lost=%u\n",
            static_cast<unsigned long long>(s.metadata),static_cast<unsigned long long>(s.maskDecoded),static_cast<unsigned long long>(s.maskNone),static_cast<unsigned long long>(s.maskRejected),
            static_cast<unsigned long long>(s.uploadAccepted),static_cast<unsigned long long>(s.uploadRefused),static_cast<unsigned long long>(s.poolUploaded),s.pending,s.copying,s.ready,s.quarantined,
            static_cast<unsigned long long>(s.pictures),static_cast<unsigned long long>(s.selected),static_cast<unsigned long long>(s.draws),static_cast<unsigned long long>(s.begins),
            miss(DrawRefusal::Unavailable),miss(DrawRefusal::Busy),miss(DrawRefusal::Command),miss(DrawRefusal::Epoch),miss(DrawRefusal::Pipeline),miss(DrawRefusal::Ineligible),miss(DrawRefusal::Descriptor),miss(DrawRefusal::Image),miss(DrawRefusal::ExactPicture),miss(DrawRefusal::ReadyMask),miss(DrawRefusal::Capacity),
            static_cast<unsigned long long>(s.guardAccepted),static_cast<unsigned long long>(s.guardAttempts),static_cast<unsigned long long>(s.guardRefused),static_cast<unsigned long long>(s.opaqueSubmits),s.pipelines,unsigned(s.lost));
        std::fprintf(stderr,"[Steam Link Native Passthrough] coverage content: fully-opaque/fully-transparent/mixed=%llu/%llu/%llu; exact pre-draw storage retained; no image capture\n",
            static_cast<unsigned long long>(s.maskOpaque),static_cast<unsigned long long>(s.maskTransparent),static_cast<unsigned long long>(s.maskMixed));
        std::fprintf(stderr,"[Steam Link Native Passthrough] descriptor flow: allocated/refused=%llu/%llu; pool-retire/set-retire=%llu/%llu; unknown-updates/copies=%llu/%llu\n",
            static_cast<unsigned long long>(s.descriptorAllocated),static_cast<unsigned long long>(s.descriptorAllocationRefused),static_cast<unsigned long long>(s.descriptorPoolRetirements),static_cast<unsigned long long>(s.descriptorSetsRetired),static_cast<unsigned long long>(s.descriptorUnknownUpdates),static_cast<unsigned long long>(s.descriptorCopies));
        const auto descriptor=[&](DescriptorRefusal r){return static_cast<unsigned long long>(s.descriptorRefusals[static_cast<std::size_t>(r)]);};
        std::fprintf(stderr,"[Steam Link Native Passthrough] renderer metadata: capacity image/memory/view/descriptor=%llu/%llu/%llu/%llu (limits=512/512/1024/256); skipped untracked writes/copies=%llu/%llu; draw descriptor missing-allocation/missing-entry/invalid-entry/missing-view/view-serial=%llu/%llu/%llu/%llu/%llu; camera draw/collect refused=%llu/%llu\n",
            static_cast<unsigned long long>(s.imageCapacityRefused),static_cast<unsigned long long>(s.memoryCapacityRefused),static_cast<unsigned long long>(s.viewCapacityRefused),static_cast<unsigned long long>(s.descriptorCapacityRefused),
            static_cast<unsigned long long>(s.descriptorUntrackedWrites),static_cast<unsigned long long>(s.descriptorUntrackedCopies),
            descriptor(DescriptorRefusal::MissingAllocation),descriptor(DescriptorRefusal::MissingEntry),descriptor(DescriptorRefusal::InvalidEntry),descriptor(DescriptorRefusal::MissingView),descriptor(DescriptorRefusal::ViewSerial),
            miss(DrawRefusal::Camera),static_cast<unsigned long long>(s.collectRefusals[static_cast<std::size_t>(CollectRefusal::Camera)]));
        const auto collect=[&](CollectRefusal r){return static_cast<unsigned long long>(s.collectRefusals[static_cast<std::size_t>(r)]);};
        const auto submit=[&](SubmitRefusal r){return static_cast<unsigned long long>(s.submitRefusals[static_cast<std::size_t>(r)]);};
        std::fprintf(stderr,"[Steam Link Native Passthrough] renderer submit: core/submit2=%llu/%llu; first-pNext/refused-pNext=%u/%u; unsupported busy/bounds/shape/extension/flags=%llu/%llu/%llu/%llu/%llu; collect accepted/attempts=%llu/%llu; collect miss cb/empty/ineligible/epoch/lease/descriptor/image/memory/capacity=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
            static_cast<unsigned long long>(s.coreSubmits),static_cast<unsigned long long>(s.submits2),s.firstNextType,s.firstRefusedNextType,
            submit(SubmitRefusal::Busy),submit(SubmitRefusal::Bounds),submit(SubmitRefusal::Shape),submit(SubmitRefusal::Extension),submit(SubmitRefusal::Flags),
            static_cast<unsigned long long>(s.collected),static_cast<unsigned long long>(s.collects),
            collect(CollectRefusal::Command),collect(CollectRefusal::Empty),collect(CollectRefusal::Ineligible),collect(CollectRefusal::Epoch),collect(CollectRefusal::Lease),collect(CollectRefusal::Descriptor),collect(CollectRefusal::Image),collect(CollectRefusal::Memory),collect(CollectRefusal::Capacity));
    }
    bool load(){
#define LOAD(n) n=reinterpret_cast<PFN_vk##n>(gdpa(device,"vk" #n));if(!n)return false;
        FUNCS(LOAD)
#undef LOAD
        return true;
    }
    void release(Command& command){
        for(std::size_t n=0;n<command.used;++n)pool.releaseRecorded(command.uses[n].lease);
        command.uses={};command.used=0;command.pipeline=command.set0=0;command.eligible=false;
    }
    void barrier(VkCommandBuffer cb,Control& control,bool before){
        VkBufferMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        memoryBarrier.buffer=control.buffer;memoryBarrier.offset=control.offset;memoryBarrier.size=16;
        memoryBarrier.srcQueueFamilyIndex=memoryBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.srcAccessMask=before?VK_ACCESS_UNIFORM_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask=before?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_UNIFORM_READ_BIT;
        CmdPipelineBarrier(cb,before?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
            before?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&memoryBarrier,0,nullptr);
    }
    bool makeControls(){
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;
        if(CreateCommandPool(device,&cp,nullptr,&guardPool)!=VK_SUCCESS)return false;
        std::array<VkCommandBuffer,129> cbs{};VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool=guardPool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=cbs.size();
        if(AllocateCommandBuffers(device,&ca,cbs.data())!=VK_SUCCESS)return false;
        zero=cbs[0];VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if(BeginCommandBuffer(zero,&begin)!=VK_SUCCESS)return false;
        // Vulkan 1.1 guarantees minUniformBufferOffsetAlignment <= 256.
        // One fixed allocation, one zero fill and two barriers per native
        // submit replace 128 redundant clears/barrier pairs on mobile GPUs.
        constexpr VkDeviceSize stride=256;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=controls.size()*stride;
        buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(CreateBuffer(device,&buffer,nullptr,&controlsBuffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements requirements{};GetBufferMemoryRequirements(device,controlsBuffer,&requirements);unsigned type=UINT32_MAX;
        for(unsigned n=0;n<memory.memoryTypeCount;++n)if(requirements.memoryTypeBits&(1u<<n)){type=n;break;}
        if(type==UINT32_MAX)return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
        if(AllocateMemory(device,&allocation,nullptr,&controlsMemory)!=VK_SUCCESS||BindBufferMemory(device,controlsBuffer,controlsMemory,0)!=VK_SUCCESS)return false;
        VkBufferMemoryBarrier all{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};all.buffer=controlsBuffer;all.size=VK_WHOLE_SIZE;
        all.srcQueueFamilyIndex=all.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        all.srcAccessMask=VK_ACCESS_UNIFORM_READ_BIT;all.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(zero,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&all,0,nullptr);
        CmdFillBuffer(zero,controlsBuffer,0,VK_WHOLE_SIZE,0);
        all.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;all.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT;
        CmdPipelineBarrier(zero,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&all,0,nullptr);
        for(std::size_t k=0;k<controls.size();++k){auto& control=controls[k];control.one=cbs[k+1];control.buffer=controlsBuffer;control.offset=k*stride;
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=controlPool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&controlLayout;
            if(AllocateDescriptorSets(device,&allocate,&control.set)!=VK_SUCCESS)return false;
            VkDescriptorBufferInfo info{control.buffer,control.offset,16};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=control.set;write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&info;UpdateDescriptorSets(device,1,&write,0,nullptr);
            // These immutable commands own their value bytes. Native flags=0
            // recordings can repeat; no host write can race a pending GPU read.
            const float on=1;
            if(BeginCommandBuffer(control.one,&begin)!=VK_SUCCESS)return false;
            barrier(control.one,control,true);CmdUpdateBuffer(control.one,control.buffer,control.offset,sizeof(on),&on);barrier(control.one,control,false);
            if(EndCommandBuffer(control.one)!=VK_SUCCESS)return false;
        }
        return EndCommandBuffer(zero)==VK_SUCCESS;
    }
    void destroyControls(){
        if(guardPool)DestroyCommandPool(device,guardPool,nullptr);
        if(controlsBuffer)DestroyBuffer(device,controlsBuffer,nullptr);
        if(controlsMemory)FreeMemory(device,controlsMemory,nullptr);
        controlsBuffer={};controlsMemory={};for(auto& control:controls)control={};
        guardPool={};zero={};
    }
    static bool admitted(void* owner) noexcept{
        auto&p=*static_cast<Impl*>(owner);return !p.lost.load(std::memory_order_acquire)&&p.admittedPipelines.load(std::memory_order_acquire)>0;
    }
    void retirePictures() noexcept {
        for(auto& slot:pictures){const auto old=std::atomic_load_explicit(&slot,std::memory_order_acquire);
            if(old&&!current(*old))std::atomic_store_explicit(&slot,std::shared_ptr<const PreparedPicture>{},std::memory_order_release);}
    }
    static void decodedCoverage(void* owner,const sb::CertifiedAccessUnit& certificate,sb::Bytes bytes,sb::CoverageRange range) noexcept{
        auto&p=*static_cast<Impl*>(owner);++p.counts.metadata;p.retirePictures();
        const auto& d=certificate.descriptor;
        if(certificate.coverageEncoding==sb::CoverageEncoding::None){++p.counts.maskNone;return;}
        try{
            // Only Process's authenticated synchronous callback supplies this
            // transient view. Its byte count still has to match this exact
            // descriptor; content range never grants picture/lease authority.
            if(!d.codedWidth||!d.codedHeight||!bytes.data||bytes.size!=std::size_t(d.codedWidth)*d.codedHeight||range.minimum>range.maximum){++p.counts.maskRejected;return;}
            ++p.counts.maskDecoded;
            if(range.minimum==255)++p.counts.maskOpaque;else if(!range.maximum)++p.counts.maskTransparent;else ++p.counts.maskMixed;
            if(p.pool.upload({certificate.binding,certificate.descriptor},d.codedWidth,d.codedHeight,bytes)){
                ++p.counts.uploadAccepted;p.uploadsPending.store(true,std::memory_order_release);
            }
            else ++p.counts.uploadRefused;
        }catch(...){++p.counts.maskRejected;}
    }
    static void coverage(void* owner,const sb::CertifiedAccessUnit& certificate) noexcept{
        if(certificate.coverageEncoding==sb::CoverageEncoding::None){decodedCoverage(owner,certificate,{},{});return;}
        try{std::vector<std::uint8_t> bytes;sb::CoverageRange range;
            if(sb::decodeCertifiedCoverage(certificate,bytes,range)){decodedCoverage(owner,certificate,{bytes.data(),bytes.size()},range);return;}
        }catch(...){}
        auto&p=*static_cast<Impl*>(owner);++p.counts.metadata;++p.counts.maskRejected;p.retirePictures();
    }
    static void picture(void* owner,const LocalPicture& value) noexcept{
        auto&p=*static_cast<Impl*>(owner);try{
            if(value.metadata.lease.slot>=p.pictures.size()||!current(value))return;
            auto& slot=p.pictures[value.metadata.lease.slot];const auto old=std::atomic_load_explicit(&slot,std::memory_order_acquire);
            const bool samePicture=old&&old->metadata.binding==value.metadata.binding&&same(old->metadata.descriptor,value.metadata.descriptor)
                &&old->metadata.lease.epoch==value.metadata.lease.epoch&&old->metadata.lease.sequence==value.metadata.lease.sequence;
            if(samePicture)return;
            const auto lease=p.pool.acquireScheduled({value.metadata.binding,value.metadata.descriptor});if(!lease)return;
            std::shared_ptr<Retention> retained;
            try{retained=std::make_shared<Retention>(p.pool,lease);}catch(...){p.pool.releaseRecorded(lease);return;}
            auto snapshot=std::make_shared<PreparedPicture>();static_cast<LocalPicture&>(*snapshot)=value;snapshot->retained=std::move(retained);
            std::atomic_store_explicit(&slot,std::shared_ptr<const PreparedPicture>(std::move(snapshot)),std::memory_order_release);++p.counts.pictures;
        }catch(...){}
    }
    static bool published(void* owner,const LocalPicture& value) noexcept{
        auto&p=*static_cast<Impl*>(owner);if(value.metadata.lease.slot>=p.pictures.size())return false;
        const auto ready=std::atomic_load_explicit(&p.pictures[value.metadata.lease.slot],std::memory_order_acquire);
        return ready&&current(*ready)&&ready->retained&&ready->metadata.binding==value.metadata.binding&&same(ready->metadata.descriptor,value.metadata.descriptor)
            &&ready->metadata.lease.epoch==value.metadata.lease.epoch&&ready->metadata.lease.sequence==value.metadata.lease.sequence;
    }
    static void tick(void* owner) noexcept{auto& p=*static_cast<Impl*>(owner);p.retirePictures();p.pool.reapCold();p.reportCold();}
    bool matchingImage(std::uint64_t image,std::shared_ptr<const LocalPicture>& output){
        const auto* i=find(images,image);if(!i)return false;const auto* m=find(memories,i->memory);if(!m||m->generation!=i->memoryGeneration||m->duplicate<0)return false;
        output.reset();
        for(const auto& slot:pictures){auto picture=std::atomic_load_explicit(&slot,std::memory_order_acquire);if(picture&&current(*picture)&&picture->metadata.lease.kernel==local::KernelIdentity{{m->device,m->inode}}&&picture->metadata.descriptor.codedWidth==i->width&&picture->metadata.descriptor.codedHeight==i->height){if(output)return false;output=std::move(picture);}}
        return output!=nullptr;
    }
    bool collect(VkCommandBuffer cb){
        ++counts.collects;
        const auto refuse=[&](CollectRefusal reason){++counts.collectRefusals[static_cast<std::size_t>(reason)];return false;};
        auto* command=find(commands,handle(cb));if(!command)return refuse(CollectRefusal::Command);
        if(!command->used)return refuse(CollectRefusal::Empty);
        if(!command->cameraEpoch||command->cameraEpoch!=cameraAlphaEpoch())return refuse(CollectRefusal::Camera);
        CollectRefusal why=CollectRefusal::Count;
        const auto require=[&](bool condition,CollectRefusal reason){if(!condition&&why==CollectRefusal::Count)why=reason;};
        require(command->eligible,CollectRefusal::Ineligible);
        require(!lost.load(std::memory_order_acquire)&&command->epoch==observationEpoch.load(std::memory_order_acquire),CollectRefusal::Epoch);
        for(std::size_t n=0;n<command->used;++n){auto& use=command->uses[n];require(current(use.picture),CollectRefusal::Lease);
            // A descriptor can legally change after recording but before
            // submission. It must still name this EXACT leased image.
            const auto* descriptor=find(descriptors,use.descriptor);
            const auto* allocation=find(descriptorAllocations,use.descriptor);
            const auto* view=descriptor&&descriptor->valid?find(views,descriptor->view):nullptr;
            const auto* image=view?find(images,view->image):nullptr;
            const auto* memory=image?find(memories,image->memory):nullptr;
            require(descriptor&&descriptor->valid&&allocation&&allocation->generation==use.descriptorGeneration,CollectRefusal::Descriptor);
            require(view&&view->serial==use.viewSerial&&descriptor->viewSerial==use.viewSerial&&
                image&&image->id==use.image&&image->generation==use.imageGeneration&&view->generation==use.imageGeneration,CollectRefusal::Image);
            require(memory&&memory->generation==use.memoryGeneration&&image->memoryGeneration==use.memoryGeneration&&
                local::KernelIdentity{{memory->device,memory->inode}}==use.picture.metadata.lease.kernel,CollectRefusal::Memory);
            bool seen=false;for(std::size_t k=0;k<submittedCount;++k)seen|=submittedLeases[k].slot==use.lease.slot&&submittedLeases[k].generation==use.lease.generation;
            if(!seen){if(submittedCount==submittedLeases.size())require(false,CollectRefusal::Capacity);else submittedLeases[submittedCount++]=use.lease;}
        }
        if(why!=CollectRefusal::Count)return refuse(why);
        ++counts.collected;return true;
    }

};
Renderer::Renderer():impl_(new Impl){}
Renderer::~Renderer()=default;
frame_pixel_ssbo_journal::Journal& Renderer::definitions() noexcept{return impl_->definitions;}
RendererStats Renderer::statsCold()const noexcept{return impl_->stats();}
void Renderer::shaderCreated(VkShaderModule value,const VkShaderModuleCreateInfo& info) noexcept{
    auto&p=*impl_;try{
        if(!p.definitions.createShader(value,info,VK_SUCCESS)||!p.prepared||info.codeSize<20||info.codeSize>65536)return;
        std::lock_guard<std::mutex> lock(p.gate);if(p.count1Authorized)return;
        auto original=std::make_unique<frame_pixel_ssbo_clone::ShaderDefinition>();
        auto proof=std::make_unique<frame_pixel_ssbo_profile::Artifact>();
        frame_pixel_ssbo_clone::Refusal why;
        if(frame_pixel_ssbo_clone::captureShader(info,frame_pixel_ssbo_clone::ShaderRole::Fragment,*original,why)&&
           frame_pixel_ssbo_profile::transform(*original,false,*proof,why)&&
           p.definitions.authorizeProbeInterface(proof->profile)){
            p.count1Authorized=true;coldNote("native count1 image interface verified");
        }else coldNote("native count1 image interface refused; native pipeline retained");
    }catch(...){coldNote("native shader admission unavailable; native pipeline retained");}
}
bool Renderer::prepareCold(VkDevice device,VkPhysicalDeviceMemoryProperties memory,PFN_vkGetDeviceProcAddr get,const VkDeviceCreateInfo& create,std::uint32_t api,std::uint32_t family) noexcept{
    auto&p=*impl_;try{
        if(p.device)return false;
        p.gdpa=get;p.device=device;
        p.QueueSubmit=reinterpret_cast<PFN_vkQueueSubmit>(get(device,"vkQueueSubmit"));
        p.QueueSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(get(device,"vkQueueSubmit2"));
        p.QueueSubmit2KHR=reinterpret_cast<PFN_vkQueueSubmit2>(get(device,"vkQueueSubmit2KHR"));
        if(!enabled())return false;
        p.device=device;p.memory=memory;p.gdpa=get;p.api=api;p.family=family;
        p.definitions.createDevice(device,create,VK_SUCCESS);
        if(processKind()==ProcessKind::Vrlink)return true;
        if(api<VK_API_VERSION_1_1){coldNote("renderer unavailable: negotiated Vulkan API below 1.1; original retained");return false;}
        if(!p.load()){coldNote("renderer unavailable: required Vulkan dispatch missing; original retained");return false;}
        // Ordinary native RGB is always the baseline. Missing exact coverage
        // never installs a picture/mask atlas decoder or replaces the picture.
        if(!readCoverageFragmentCold(p.coverageWords)){coldNote("exact coverage unavailable: coverage.spv missing, changed or inaccessible; ordinary native video retained");return false;}
        if(family==UINT32_MAX){coldNote("exact coverage unavailable: no admitted graphics queue; ordinary native video retained");return false;}
        if(!p.pool.prepare(device,get,memory,family)){coldNote("exact coverage unavailable: GPU pool preparation failed; ordinary native video retained");return false;}
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=1;layout.pBindings=&binding;
        if(p.CreateDescriptorSetLayout(device,&layout,nullptr,&p.controlLayout)!=VK_SUCCESS){coldNote("exact coverage unavailable: control layout creation failed; ordinary native video retained");return false;}
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,128};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;pool.maxSets=128;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        if(p.CreateDescriptorPool(device,&pool,nullptr,&p.controlPool)!=VK_SUCCESS){coldNote("exact coverage unavailable: control pool creation failed; ordinary native video retained");return false;}
        // Exactly one requested nonprotected queue in this admitted graphics
        // family. Other families cannot record one of our eligible primaries.
        unsigned queues=0;for(unsigned n=0;n<create.queueCreateInfoCount;++n)if(create.pQueueCreateInfos[n].queueFamilyIndex==family&&!create.pQueueCreateInfos[n].flags)queues+=create.pQueueCreateInfos[n].queueCount;
        if(queues!=1){coldNote("exact coverage unavailable: graphics family does not request exactly one queue; ordinary native video retained");return false;}
        if(!p.makeControls()){coldNote("exact coverage unavailable: immutable control preparation failed; ordinary native video retained");return false;}
        p.GetDeviceQueue(device,family,0,&p.queue);if(!p.queue){coldNote("exact coverage unavailable: native graphics queue missing; ordinary native video retained");return false;}
        p.prepared=p.coveragePrepared=true;coldNote("native full-colour exact-coverage resources verified");return true;
    }catch(...){coldNote("renderer preparation failed; exact coverage disabled");return p.coveragePrepared;}
}
void Renderer::graphicsCreated(VkPipelineCache cache,const VkGraphicsPipelineCreateInfo& info,const VkAllocationCallbacks* allocator,VkPipeline& original) noexcept{
    auto&p=*impl_;if(!p.coveragePrepared)return;
    // Native profile uses the default allocator. A caller-owned allocation
    // context has no retained lifetime here, so keep that unsupported object
    // entirely native instead of borrowing callbacks after this cold call.
    if(allocator){coldNote("custom pipeline allocator retained without replacement");return;}
    try{journal::Event event;if(!p.definitions.createPipeline(original,info,VK_SUCCESS,&event)){
            if(event.contract.reason==frame_pixel_ssbo_clone::Reason::Image9Extent)coldNote("native pipeline refused: image interface not verified");
            return;
        }auto ready=std::make_unique<journal::Ready>();if(!p.definitions.lookupPipeline({handle(p.device),event.value,event.generation},*ready))return;
        if(frame_native_presenter::inspectCompletedPipeline(p.device,p.api,VK_SUCCESS,info,original,ready->recipe).reason!=frame_native_presenter::Reason::None)return;
        std::lock_guard<std::mutex> lock(p.gate);auto* entry=add(p.pipelines,handle(original));if(!entry)return;
        auto create=[&](const std::vector<std::uint32_t>& words,VkPipelineLayout layout,VkPipeline& pipeline){
            VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=words.size()*4;shader.pCode=words.data();VkShaderModule module{};if(p.CreateShaderModule(p.device,&shader,nullptr,&module)!=VK_SUCCESS)return;
            auto candidate=info;std::array<VkPipelineShaderStageCreateInfo,2> stages{{info.pStages[0],info.pStages[1]}};for(auto& stage:stages)if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT)stage.module=module;candidate.pStages=stages.data();candidate.layout=layout;
            const auto result=p.CreateGraphicsPipelines(p.device,cache,1,&candidate,nullptr,&pipeline);p.DestroyShaderModule(p.device,module,nullptr);if(pipeline==original){pipeline={};return;}if(result!=VK_SUCCESS&&pipeline){p.DestroyPipeline(p.device,pipeline,nullptr);pipeline={};}
        };
        // The caller retains its exact native pipeline handle. A proved
        // current picture may bind the companion only for that recorded draw.
        VkDescriptorSetLayout layouts[3]{from<VkDescriptorSetLayout>(ready->recipe.provenance[0].value),p.pool.descriptorLayout(),p.controlLayout};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};ci.setLayoutCount=3;ci.pSetLayouts=layouts;ci.pushConstantRangeCount=ready->recipe.layout.count;ci.pPushConstantRanges=ready->recipe.layout.ranges.data();
        VkPipelineLayout coverageLayout{};
        if(p.CreatePipelineLayout(p.device,&ci,nullptr,&coverageLayout)==VK_SUCCESS&&coverageLayout){
            entry->layout=coverageLayout;create(p.coverageWords,entry->layout,entry->coverage);
        }
        if(entry->coverage){
            p.admittedPipelines.fetch_add(1,std::memory_order_release);
            if(!p.registered)p.registered=registerConsumerCold({&p,Impl::coverage,Impl::picture,Impl::tick,Impl::admitted,Impl::published,Impl::decodedCoverage});
            coldNote("native exact-coverage pipeline prepared; original picture preserved");
        }else{if(entry->layout)p.DestroyPipelineLayout(p.device,entry->layout,nullptr);*entry={};coldNote("exact-coverage pipeline unavailable; ordinary native video retained");}
    }catch(...){/* The successfully created native pipeline remains unchanged. */}
}
void Renderer::pipelineDestroyed(VkPipeline original) noexcept{
    auto&p=*impl_;try{std::lock_guard<std::mutex> lock(p.gate);if(auto* entry=find(p.pipelines,handle(original))){
        p.definitions.destroyPipeline(original);
        if(entry->coverage)p.admittedPipelines.fetch_sub(1,std::memory_order_release);
        if(entry->coverage)p.DestroyPipeline(p.device,entry->coverage,nullptr);
        if(entry->layout)p.DestroyPipelineLayout(p.device,entry->layout,nullptr);
        // The downstream original vkDestroyPipeline owns its native pipeline.
        *entry={};
    }else p.definitions.destroyPipeline(original);}catch(...){}
}
void Renderer::imageCreated(VkImage value,const VkImageCreateInfo& info) noexcept{
    auto&p=*impl_;bool dma=false;auto* node=static_cast<const VkBaseInStructure*>(info.pNext);
    for(unsigned n=0;node&&n<32;++n,node=node->pNext)if(node->sType==VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)dma=(reinterpret_cast<const VkExternalMemoryImageCreateInfo*>(node)->handleTypes&VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)!=0;
    if(auto* source=sourceImages())source->created(handle(p.device),handle(value),{info.extent.width,info.extent.height,std::uint32_t(info.format),dma});
    if(!p.prepared||!dma||info.imageType!=VK_IMAGE_TYPE_2D||info.samples!=VK_SAMPLE_COUNT_1_BIT||info.mipLevels!=1||info.arrayLayers!=1)return;
    std::lock_guard<std::mutex> lock(p.gate);if(auto* image=add(p.images,handle(value))){image->generation=++p.generation;image->width=info.extent.width;image->height=info.extent.height;}else if(!find(p.images,handle(value)))++p.counts.imageCapacityRefused;
}
void Renderer::imageDestroyed(VkImage value) noexcept{auto&p=*impl_;if(auto* source=sourceImages())source->destroyed(handle(p.device),handle(value));std::lock_guard<std::mutex> lock(p.gate);if(auto* image=find(p.images,handle(value)))*image={};}
MemoryPreparation Renderer::memoryPreparing(const VkMemoryAllocateInfo& info) noexcept{
    MemoryPreparation result;if(!impl_->prepared)return result;
    auto* node=static_cast<const VkBaseInStructure*>(info.pNext);
    for(unsigned n=0;node&&n<32;++n,node=node->pNext)if(node->sType==VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR){
        const auto& imported=*reinterpret_cast<const VkImportMemoryFdInfoKHR*>(node);
        if(imported.handleType!=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT||imported.fd<0)return result;
        // The original allocation consumes its fd on SUCCESS. Retain this exact
        // kernel object BEFORE forwarding, never stat the potentially reused fd.
        result.duplicate=fcntl(imported.fd,F_DUPFD_CLOEXEC,3);struct stat file{};
        if(result.duplicate<0||fstat(result.duplicate,&file)){if(result.duplicate>=0)close(result.duplicate);result={};return result;}
        result.device=std::uint64_t(file.st_dev);result.inode=std::uint64_t(file.st_ino);return result;
    }return result;
}
void Renderer::memoryAllocated(VkDeviceMemory value,const VkMemoryAllocateInfo& info,MemoryPreparation& preparation,VkResult original) noexcept{
    auto&p=*impl_;if(auto* source=sourceImages()){bool exported=false;auto* node=static_cast<const VkBaseInStructure*>(info.pNext);for(unsigned n=0;node&&n<32;++n,node=node->pNext)if(node->sType==VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)exported=(reinterpret_cast<const VkExportMemoryAllocateInfo*>(node)->handleTypes&VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)!=0;if(original==VK_SUCCESS)source->allocated(handle(p.device),handle(value),exported);}
    if(preparation.duplicate<0)return;
    std::lock_guard<std::mutex> lock(p.gate);auto* memory=original==VK_SUCCESS?add(p.memories,handle(value)):nullptr;
    if(memory){memory->generation=++p.generation;memory->device=preparation.device;memory->inode=preparation.inode;memory->duplicate=preparation.duplicate;preparation.duplicate=-1;}
    if(original==VK_SUCCESS&&!memory&&!find(p.memories,handle(value)))++p.counts.memoryCapacityRefused;
    if(preparation.duplicate>=0)close(preparation.duplicate);
    preparation={};
}
void Renderer::memoryFreed(VkDeviceMemory value) noexcept{auto&p=*impl_;if(auto* source=sourceImages())source->freed(handle(p.device),handle(value));std::lock_guard<std::mutex> lock(p.gate);if(auto* memory=find(p.memories,handle(value))){if(memory->duplicate>=0)close(memory->duplicate);*memory={};}}
void Renderer::imageBound(VkImage value,VkDeviceMemory memory,VkDeviceSize offset) noexcept{auto&p=*impl_;if(auto* source=sourceImages())source->bound(handle(p.device),handle(value),handle(memory),offset);std::lock_guard<std::mutex> lock(p.gate);auto* image=find(p.images,handle(value));const auto* allocation=find(p.memories,handle(memory));if(image&&allocation&&!offset){image->memory=allocation->id;image->memoryGeneration=allocation->generation;}}
void Renderer::memoryExported(VkDeviceMemory memory,int fd,VkExternalMemoryHandleTypeFlagBits type,std::uintptr_t caller) noexcept{auto&p=*impl_;if(auto* source=sourceImages())if(source->exported(handle(p.device),handle(memory),fd,std::uint32_t(type),caller))sourceExportedFd(fd);}
void Renderer::viewCreated(VkImageView value,const VkImageViewCreateInfo& info) noexcept{auto&p=*impl_;if(!p.prepared)return;std::lock_guard<std::mutex> lock(p.gate);const auto* image=find(p.images,handle(info.image));if(!image||info.viewType!=VK_IMAGE_VIEW_TYPE_2D||info.subresourceRange.baseMipLevel||info.subresourceRange.levelCount!=1||info.subresourceRange.baseArrayLayer||info.subresourceRange.layerCount!=1)return;if(auto* view=add(p.views,handle(value))){view->image=image->id;view->generation=image->generation;view->serial=++p.generation;}else if(!find(p.views,handle(value)))++p.counts.viewCapacityRefused;}
void Renderer::viewDestroyed(VkImageView value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);if(auto* view=find(p.views,handle(value)))*view={};}
void Renderer::descriptorWrites(std::uint32_t count,const VkWriteDescriptorSet* writes,std::uint32_t copies,const VkCopyDescriptorSet* copy) noexcept{
    auto&p=*impl_;if(!p.prepared)return;std::lock_guard<std::mutex> lock(p.gate);
    if((count&&!writes)||(copies&&!copy)){for(auto& entry:p.descriptors)entry.valid=false;++p.counts.descriptorUnknownUpdates;return;}
    // Follow the original API batch in fixed chunks without another allocation.
    // A large UI update is not evidence that unrelated video sets changed.
    for(std::size_t first=0;first<count;first+=256)for(std::size_t n=first;n<std::min<std::size_t>(count,first+256);++n){const auto& write=writes[n];if(write.dstBinding!=9||write.dstArrayElement)continue;
        const auto* allocation=find(p.descriptorAllocations,handle(write.dstSet));
        auto* entry=find(p.descriptors,handle(write.dstSet));
        if(!allocation){if(entry)*entry={};++p.counts.descriptorUnknownUpdates;continue;}
        const bool supported=write.sType==VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET&&!write.pNext&&write.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE&&write.descriptorCount&&write.descriptorCount<=3&&write.pImageInfo;
        const auto* view=supported?find(p.views,handle(write.pImageInfo[0].imageView)):nullptr;
        // An unrelated UI view is not a video descriptor. Do not reserve empty
        // metadata for it; overwriting this exact set still retires its old use.
        if(!view){if(entry)*entry={};if(supported)++p.counts.descriptorUntrackedWrites;else ++p.counts.descriptorUnknownUpdates;continue;}
        if(!entry)entry=add(p.descriptors,handle(write.dstSet));
        if(!entry){++p.counts.descriptorCapacityRefused;continue;}
        *entry={handle(write.dstSet),view->id,view->serial,true};
    }
    // Vulkan applies writes first and then copies in the supplied order.
    for(std::size_t first=0;first<copies;first+=256)for(std::size_t n=first;n<std::min<std::size_t>(copies,first+256);++n){const auto& update=copy[n];++p.counts.descriptorCopies;
        if(update.dstBinding!=9||update.dstArrayElement)continue;
        const auto* source=find(p.descriptors,handle(update.srcSet));
        const auto* sourceAllocation=find(p.descriptorAllocations,handle(update.srcSet));
        const auto* destinationAllocation=find(p.descriptorAllocations,handle(update.dstSet));
        // Save before creating/overwriting dst; src and dst may name one set.
        const auto prior=source?*source:Impl::Descriptor{};
        auto* destination=find(p.descriptors,handle(update.dstSet));
        if(!destinationAllocation){if(destination)*destination={};++p.counts.descriptorUnknownUpdates;continue;}
        const bool supported=update.sType==VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET&&!update.pNext&&update.srcBinding==9&&!update.srcArrayElement&&update.descriptorCount&&update.descriptorCount<=3&&sourceAllocation;
        const auto* view=supported&&prior.valid?find(p.views,prior.view):nullptr;
        if(!view||view->serial!=prior.viewSerial){if(destination)*destination={};if(supported)++p.counts.descriptorUntrackedCopies;else ++p.counts.descriptorUnknownUpdates;continue;}
        if(!destination)destination=add(p.descriptors,handle(update.dstSet));
        if(!destination){++p.counts.descriptorCapacityRefused;continue;}
        *destination={handle(update.dstSet),prior.view,prior.viewSerial,true};
    }
}
void Renderer::descriptorsAllocated(const VkDescriptorSetAllocateInfo& info,const VkDescriptorSet* values) noexcept{
    // Allocation lifetime begins at native device startup, independently of
    // source admission, shader discovery, or whether MR has ever been active.
    auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);
    if(info.sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO||!info.descriptorPool||(info.descriptorSetCount&&!values)){++p.counts.descriptorAllocationRefused;return;}
    for(std::size_t first=0;first<info.descriptorSetCount;first+=256)for(std::size_t n=first;n<std::min<std::size_t>(info.descriptorSetCount,first+256);++n){
        const auto id=handle(values[n]);if(auto* old=find(p.descriptors,id))*old={};
        auto* entry=id?add(p.descriptorAllocations,id):nullptr;
        if(!entry||p.descriptorGeneration==~std::uint64_t(0)){if(entry)*entry={};++p.counts.descriptorAllocationRefused;continue;}
        *entry={id,handle(info.descriptorPool),++p.descriptorGeneration};++p.counts.descriptorAllocated;
    }
}
void Renderer::descriptorRetired(VkDescriptorSet value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);bool known=false;if(auto* entry=find(p.descriptors,handle(value))){*entry={};known=true;}if(auto* entry=find(p.descriptorAllocations,handle(value))){*entry={};known=true;}if(known)++p.counts.descriptorSetsRetired;}
void Renderer::descriptorPoolRetired(VkDescriptorPool pool) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);++p.counts.descriptorPoolRetirements;for(auto& allocation:p.descriptorAllocations)if(allocation.id&&allocation.pool==handle(pool)){if(auto* entry=find(p.descriptors,allocation.id))*entry={};allocation={};++p.counts.descriptorSetsRetired;}}
void Renderer::descriptorInvalidated(VkDescriptorSet value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);if(auto* entry=find(p.descriptors,handle(value)))entry->valid=false;++p.counts.descriptorUnknownUpdates;}
void Renderer::descriptorsInvalidated() noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);for(auto& entry:p.descriptors)entry={};}
void Renderer::queueObserved(VkQueue,std::uint32_t) noexcept{}
void Renderer::commandAllocated(VkCommandBuffer value,VkCommandBufferLevel level,std::uint32_t family) noexcept{
    auto&p=*impl_;if(!p.prepared)return;try{std::lock_guard<std::mutex> lock(p.gate);if(auto* command=add(p.commands,handle(value))){command->primary=level==VK_COMMAND_BUFFER_LEVEL_PRIMARY&&family==p.family;if(command->primary){for(auto& control:p.controls)if(!control.assigned){control.assigned=true;command->control=&control;break;}if(!command->control)command->primary=false;}}}catch(...){}
}
void Renderer::commandRetired(VkCommandBuffer value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);if(auto* command=find(p.commands,handle(value))){p.release(*command);if(command->control)command->control->assigned=false;*command={};}}
void Renderer::commandBegin(VkCommandBuffer value,const VkCommandBufferBeginInfo* info) noexcept{
    auto&p=*impl_;++p.counts.begins;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}
    if(auto* command=find(p.commands,handle(value))){p.release(*command);command->eligible=command->primary&&info&&info->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO&&!info->pNext&&
        !(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);command->epoch=p.observationEpoch.load(std::memory_order_acquire);
        // An A arriving between stereo draws must never authorize half a CB.
        command->cameraEpoch=cameraAlphaEpoch();}
}
void Renderer::pipelineBound(VkCommandBuffer value,VkPipelineBindPoint point,VkPipeline pipeline) noexcept{auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}if(auto* command=find(p.commands,handle(value))){command->pipeline=point==VK_PIPELINE_BIND_POINT_GRAPHICS?handle(pipeline):0;}}
void Renderer::setsBound(VkCommandBuffer value,VkPipelineBindPoint point,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamic) noexcept{auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}if(auto* command=find(p.commands,handle(value))){if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS||dynamic){command->set0=0;return;}if(!first&&count&&sets)command->set0=handle(sets[0]);}}
void Renderer::commandUnknown(VkCommandBuffer value) noexcept{auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}if(auto* command=find(p.commands,handle(value))){command->eligible=false;command->pipeline=command->set0=0;}}
Draw Renderer::beforeDraw(VkCommandBuffer value) noexcept{
    auto&p=*impl_;++p.counts.draws;Draw out;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    if(!lock){p.refused(DrawRefusal::Busy);return out;}
    if(!p.prepared||p.lost.load(std::memory_order_acquire)){p.refused(DrawRefusal::Unavailable);return out;}
    auto* command=find(p.commands,handle(value));if(!command){p.refused(DrawRefusal::Command);return out;}
    if(!command->cameraEpoch||command->cameraEpoch!=cameraAlphaEpoch()){p.refused(DrawRefusal::Camera);return out;}
    if(command->epoch!=p.observationEpoch.load(std::memory_order_acquire)){p.refused(DrawRefusal::Epoch);return out;}
    auto* pipeline=find(p.pipelines,command->pipeline);if(!pipeline){p.refused(DrawRefusal::Pipeline);return out;}
    out.original=from<VkPipeline>(pipeline->id);
    if(!command->eligible||!command->control||!pipeline->coverage){p.refused(DrawRefusal::Ineligible);return out;}
    const auto descriptorMiss=[&](DescriptorRefusal why,DrawRefusal reason){++p.counts.descriptorRefusals[static_cast<std::size_t>(why)];p.refused(reason);};
    const auto* descriptor=find(p.descriptors,command->set0);const auto* descriptorAllocation=find(p.descriptorAllocations,command->set0);
    if(!descriptorAllocation){descriptorMiss(DescriptorRefusal::MissingAllocation,DrawRefusal::Descriptor);return out;}
    if(!descriptor){descriptorMiss(DescriptorRefusal::MissingEntry,DrawRefusal::Descriptor);return out;}
    if(!descriptor->valid){descriptorMiss(DescriptorRefusal::InvalidEntry,DrawRefusal::Descriptor);return out;}
    const auto* view=find(p.views,descriptor->view);
    if(!view){descriptorMiss(DescriptorRefusal::MissingView,DrawRefusal::Image);return out;}
    if(view->serial!=descriptor->viewSerial){descriptorMiss(DescriptorRefusal::ViewSerial,DrawRefusal::Image);return out;}
    const auto* image=find(p.images,view->image);if(!image||image->generation!=view->generation){p.refused(DrawRefusal::Image);return out;}
    std::shared_ptr<const LocalPicture> picture;if(!p.matchingImage(image->id,picture)){p.refused(DrawRefusal::ExactPicture);return out;}
    Impl::Use* use=nullptr;
    for(unsigned n=0;n<command->used;++n)if(command->uses[n].descriptor==command->set0&&command->uses[n].descriptorGeneration==descriptorAllocation->generation&&command->uses[n].viewSerial==view->serial&&command->uses[n].picture.metadata.binding==picture->metadata.binding&&same(command->uses[n].picture.metadata.descriptor,picture->metadata.descriptor)){use=&command->uses[n];break;}
    if(!use){if(command->used==command->uses.size()){p.refused(DrawRefusal::Capacity);return out;}auto lease=p.pool.acquireScheduled({picture->metadata.binding,picture->metadata.descriptor});if(!lease){p.refused(DrawRefusal::ReadyMask);return out;}use=&command->uses[command->used++];use->lease=lease;use->picture=*picture;use->descriptor=command->set0;use->descriptorGeneration=descriptorAllocation->generation;use->viewSerial=view->serial;use->image=image->id;use->imageGeneration=image->generation;use->memoryGeneration=image->memoryGeneration;}
    // Publish before the wrapper can record the companion bind/draw. A submit
    // observing false cannot contain an earlier guarded recording; recording
    // and submitting the same CB concurrently is outside native Vulkan rules.
    p.guardedRecordingPossible.store(true,std::memory_order_release);
    out.replacement=pipeline->coverage;out.layout=pipeline->layout;out.coverage=use->lease.set;out.control=command->control->set;out.ready=true;++p.counts.selected;return out;
}
void Renderer::commandReset(VkCommandBuffer value) noexcept{
    auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}
    if(auto* command=find(p.commands,handle(value)))p.release(*command);
}
void Renderer::trackingLost() noexcept{impl_->lost.store(true,std::memory_order_release);}
VkResult Renderer::submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* infos,VkFence fence) noexcept{
    auto&p=*impl_;if(!p.QueueSubmit)return VK_ERROR_INITIALIZATION_FAILED;
    // Authenticated coverage may arrive before the independent camera A ack.
    // Submit its owned copies without requiring a prior masked recording, so
    // finite Pending slots can complete and be reused while the camera warms.
    if(p.coveragePrepared&&queue==p.queue&&p.uploadsPending.exchange(false,std::memory_order_acq_rel))p.pool.drainUploads(queue);
    if(!p.coveragePrepared||queue!=p.queue||!p.guardedRecordingPossible.load(std::memory_order_acquire))return p.QueueSubmit(queue,count,infos,fence);
    ++p.counts.coreSubmits;
    // Same native queue owner, bounded stack-only scratch, original semaphores,
    // pNext and fence retained. A leading immutable zero command makes even a
    // reused flags=0 CB fail closed. Only current exact leases get a one guard.
    std::array<VkSubmitInfo,65> submits{};std::array<VkCommandBuffer,1024> commands{};
    submits[0].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submits[0].commandBufferCount=1;submits[0].pCommandBuffers=&p.zero;
    std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    bool supported=lock.owns_lock()&&count<=64&&(infos||!count);std::size_t used=0,guards=0;p.submittedCount=0;
    if(!supported)p.submitRefused(!lock.owns_lock()?SubmitRefusal::Busy:count>64?SubmitRefusal::Bounds:SubmitRefusal::Shape);
    if(supported)for(unsigned n=0;n<count;++n){
        const auto& info=infos[n];std::uint32_t nextType=0;
        const auto timeline=stream_bridge_submit::canonicalTimeline(info,nextType);p.nextObserved(nextType);
        SubmitRefusal why=SubmitRefusal::Count;
        if(timeline==stream_bridge_submit::TimelineStatus::Extension)why=SubmitRefusal::Extension;
        else if(timeline==stream_bridge_submit::TimelineStatus::Shape||info.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO||
            (!info.pCommandBuffers&&info.commandBufferCount)||(info.waitSemaphoreCount&&(!info.pWaitSemaphores||!info.pWaitDstStageMask))||
            (info.signalSemaphoreCount&&!info.pSignalSemaphores))why=SubmitRefusal::Shape;
        else if(info.commandBufferCount>512||info.waitSemaphoreCount>64||info.signalSemaphoreCount>64||used+info.commandBufferCount*2>commands.size())why=SubmitRefusal::Bounds;
        if(why!=SubmitRefusal::Count){p.submitRefused(why,nextType);supported=false;break;}
        submits[n+1]=infos[n];submits[n+1].pCommandBuffers=commands.data()+used;submits[n+1].commandBufferCount=0;
        for(unsigned k=0;k<infos[n].commandBufferCount;++k){const auto cb=infos[n].pCommandBuffers[k];
            if(p.collect(cb)){auto* command=find(p.commands,handle(cb));if(command&&command->control){commands[used++]=command->control->one;++submits[n+1].commandBufferCount;++guards;}}
            commands[used++]=cb;++submits[n+1].commandBufferCount;
        }
    }
    if(!supported){
        ++p.counts.opaqueSubmits;
        // Unknown extension/cardinality never reuses an old successful guard.
        // The owned zero CB is immutable SIMULTANEOUS_USE and kept until idle.
        // Discard scratch one-guards; the original native CBs run after zero.
        // Their recording references still retain every sampled descriptor
        // until a native-completed reset/retirement. No coverage sampling
        // reservation was made, so unrelated future slots remain usable.
        if(count<=64&&(infos||!count)){
            for(unsigned n=0;n<count;++n)submits[n+1]=infos[n];
            return p.QueueSubmit(queue,count+1,submits.data(),fence);
        }
        if(!infos)return p.QueueSubmit(queue,count,infos,fence);
        // Large batches keep every original submit and its extension objects.
        // The caller still owns the same queue's external synchronization for
        // this whole call. Stack-only chunks preserve submission order and put
        // the original completion fence on the final chunk only.
        for(std::uint32_t at=0;at<count;){const auto chunk=std::min<std::uint32_t>(64,count-at);
            for(unsigned n=0;n<chunk;++n)submits[n+1]=infos[at+n];
            const auto result=p.QueueSubmit(queue,chunk+1,submits.data(),at+chunk==count?fence:VK_NULL_HANDLE);
            if(result!=VK_SUCCESS)return result;
            at+=chunk;
        }
        return VK_SUCCESS;
    }
    p.counts.guardAttempts.fetch_add(guards,std::memory_order_relaxed);
    const auto reserved=p.pool.prepareSubmit(queue,p.submittedLeases.data(),p.submittedCount);
    if(!reserved){
        // Fenced retirement is part of admission, not just resource accounting.
        // Clear all one guards on refusal while retaining ordinary native RGB.
        for(std::size_t n=0;n<used;++n)for(const auto& control:p.controls)if(commands[n]==control.one)commands[n]=p.zero;
    }
    const auto result=p.QueueSubmit(queue,count+1,submits.data(),fence);
    if(reserved&&result==VK_SUCCESS)p.counts.guardAccepted.fetch_add(guards,std::memory_order_relaxed);
    else p.counts.guardRefused.fetch_add(guards,std::memory_order_relaxed);
    if(!guards||!reserved)++p.counts.opaqueSubmits;
    if(reserved)p.pool.submitted(reserved,queue,p.submittedLeases.data(),p.submittedCount,result);
    p.submittedCount=0;return result;
}
VkResult Renderer::submit2(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* infos,VkFence fence,bool khr) noexcept{
    auto&p=*impl_;const auto forward=khr?p.QueueSubmit2KHR:p.QueueSubmit2;if(!forward)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(p.coveragePrepared&&queue==p.queue&&p.uploadsPending.exchange(false,std::memory_order_acq_rel))p.pool.drainUploads(queue);
    if(!p.coveragePrepared||queue!=p.queue||!p.guardedRecordingPossible.load(std::memory_order_acquire))return forward(queue,count,infos,fence);
    ++p.counts.submits2;
    std::array<VkSubmitInfo2,65> submits{};std::array<VkCommandBufferSubmitInfo,1024> commands{};
    const VkCommandBufferSubmitInfo zero{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,nullptr,p.zero,1};
    submits[0].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2;submits[0].commandBufferInfoCount=1;submits[0].pCommandBufferInfos=&zero;
    std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);bool supported=lock.owns_lock()&&count<=64&&(infos||!count);std::size_t used=0,guards=0;p.submittedCount=0;
    if(!supported)p.submitRefused(!lock.owns_lock()?SubmitRefusal::Busy:count>64?SubmitRefusal::Bounds:SubmitRefusal::Shape);
    if(supported)for(unsigned n=0;n<count;++n){
        const auto& info=infos[n];const auto nextType=info.pNext?static_cast<std::uint32_t>(static_cast<const VkBaseInStructure*>(info.pNext)->sType):0;p.nextObserved(nextType);
        if(info.pNext||info.flags||info.commandBufferInfoCount>512||(!info.pCommandBufferInfos&&info.commandBufferInfoCount)||used+info.commandBufferInfoCount*2>commands.size()){
            p.submitRefused(info.pNext?SubmitRefusal::Extension:info.flags?SubmitRefusal::Flags:(!info.pCommandBufferInfos&&info.commandBufferInfoCount)?SubmitRefusal::Shape:SubmitRefusal::Bounds,nextType);supported=false;break;}
        submits[n+1]=infos[n];submits[n+1].pCommandBufferInfos=commands.data()+used;submits[n+1].commandBufferInfoCount=0;
        for(unsigned k=0;k<infos[n].commandBufferInfoCount;++k){const auto& cb=infos[n].pCommandBufferInfos[k];
            if(p.collect(cb.commandBuffer)){auto* command=find(p.commands,handle(cb.commandBuffer));if(command&&command->control){commands[used++]={VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,nullptr,command->control->one,1};++submits[n+1].commandBufferInfoCount;++guards;}}
            commands[used++]=cb;++submits[n+1].commandBufferInfoCount;
        }
    }
    if(!supported){
        ++p.counts.opaqueSubmits;
        // Only original CBs follow zero. Their recording references retain
        // immutable storage; do not invent a failed in-flight lease reservation
        // that would permanently quarantine every future coverage slot.
        if(count<=64&&(infos||!count)){for(unsigned n=0;n<count;++n)submits[n+1]=infos[n];return forward(queue,count+1,submits.data(),fence);}
        if(!infos)return forward(queue,count,infos,fence);
        for(std::uint32_t at=0;at<count;){const auto chunk=std::min<std::uint32_t>(64,count-at);
            for(unsigned n=0;n<chunk;++n)submits[n+1]=infos[at+n];
            const auto result=forward(queue,chunk+1,submits.data(),at+chunk==count?fence:VK_NULL_HANDLE);
            if(result!=VK_SUCCESS)return result;
            at+=chunk;
        }
        return VK_SUCCESS;
    }
    p.counts.guardAttempts.fetch_add(guards,std::memory_order_relaxed);
    const auto reserved=p.pool.prepareSubmit(queue,p.submittedLeases.data(),p.submittedCount);
    if(!reserved){
        for(std::size_t n=0;n<used;++n)for(const auto& control:p.controls)if(commands[n].commandBuffer==control.one)commands[n].commandBuffer=p.zero;
    }
    const auto result=forward(queue,count+1,submits.data(),fence);
    if(reserved&&result==VK_SUCCESS)p.counts.guardAccepted.fetch_add(guards,std::memory_order_relaxed);
    else p.counts.guardRefused.fetch_add(guards,std::memory_order_relaxed);
    if(!guards||!reserved)++p.counts.opaqueSubmits;
    if(reserved)p.pool.submitted(reserved,queue,p.submittedLeases.data(),p.submittedCount,result);
    p.submittedCount=0;return result;
}
void Renderer::stopCold() noexcept{
    auto&p=*impl_;if(p.registered){unregisterConsumerCold(&p);p.registered=false;}
    std::lock_guard<std::mutex> lock(p.gate);if(!p.device)return;
    for(auto& picture:p.pictures)std::atomic_store_explicit(&picture,std::shared_ptr<const Impl::PreparedPicture>{},std::memory_order_release);
    // Device destruction is a cold lifecycle boundary. Join the producer by
    // unregistering first, then obtain actual GPU idle before owned cleanup.
    if(p.DeviceWaitIdle&&p.DeviceWaitIdle(p.device)==VK_SUCCESS){
        for(auto& command:p.commands)p.release(command);
        p.pool.destroyAfterIdle();p.destroyControls();
        for(auto& entry:p.pipelines){if(entry.coverage)p.DestroyPipeline(p.device,entry.coverage,nullptr);if(entry.layout)p.DestroyPipelineLayout(p.device,entry.layout,nullptr);entry={};}
        if(p.controlPool)p.DestroyDescriptorPool(p.device,p.controlPool,nullptr);
        if(p.controlLayout)p.DestroyDescriptorSetLayout(p.device,p.controlLayout,nullptr);
    }else p.pool.abandonAfterDeviceDestroy(); // Native DestroyDevice owns eventual reclamation.
    for(auto& memory:p.memories)if(memory.duplicate>=0)close(memory.duplicate);
    p.prepared=p.coveragePrepared=false;p.device={};
}
} // namespace stream_bridge_renderer
