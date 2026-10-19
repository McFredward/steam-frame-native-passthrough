#include "stream_bridge_coverage_gpu.h"
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace stream_bridge_coverage_gpu {
namespace {
bool same(const FrameIdentity&a,const FrameIdentity&b) noexcept {
    return a.binding==b.binding&&a.descriptor.sourceFrame==b.descriptor.sourceFrame&&
        a.descriptor.encoderGeneration==b.descriptor.encoderGeneration&&
        a.descriptor.codedWidth==b.descriptor.codedWidth&&a.descriptor.codedHeight==b.descriptor.codedHeight;
}
bool valid(const FrameIdentity&f,std::uint32_t w,std::uint32_t h,stream_bridge::Bytes b) noexcept {
    bool nonce=false;for(auto x:f.binding)nonce|=x!=0;
    return nonce&&f.descriptor.sourceFrame&&f.descriptor.encoderGeneration&&w&&h&&
        w==f.descriptor.codedWidth&&h==f.descriptor.codedHeight&&b.data&&
        std::uint64_t(w)*h==b.size&&b.size<=stream_bridge::MaximumCoverageBytes;
}
constexpr std::uint64_t RefMask=32767,Writer=32768,GenerationLimit=(std::numeric_limits<std::uint64_t>::max()>>16);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"queue completion requires lock-free ticket publication");
std::atomic<std::uint64_t> ownerSerial{1};
std::uint64_t nextOwner() noexcept {
    // Cold construction only. Do not recycle an owner epoch even if an old
    // object address is reused, and permanently refuse on counter exhaustion.
    auto serial=ownerSerial.load(std::memory_order_relaxed);
    while(serial!=(std::numeric_limits<std::uint64_t>::max()))
        if(ownerSerial.compare_exchange_weak(serial,serial+1,std::memory_order_relaxed))return serial;
    return 0;
}
}
struct Pool::Impl {
#define GPU_FUNCTIONS(X) \
 X(GetDeviceQueue) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateSampler) X(DestroySampler) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) \
 X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
 X(BindBufferMemory) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(QueueSubmit)
#define MEMBER(n) PFN_vk##n n=nullptr;
    GPU_FUNCTIONS(MEMBER)
#undef MEMBER
    enum class State { Free,Allocating,Pending,Submitting,Copying,Ready };
    struct Slot {
        std::atomic<State> state{State::Free};FrameIdentity frame{};std::uint64_t generation=0;
        std::atomic<std::uint64_t> refs{0};std::atomic<bool> quarantine{false};
        std::atomic<std::uint32_t> gpuRefs{0};VkImage image{};VkDeviceMemory imageMemory{};
        VkImageView view{};VkBuffer staging{};VkDeviceMemory stagingMemory{};
        void* mapped=nullptr;std::size_t bytes=0;std::uint32_t width=0,height=0;bool coherent=false;std::atomic<bool> initializedShaderReadLayout{false};VkDescriptorSet set{};
        VkCommandBuffer copy{};VkFence uploaded{};
    };
    enum class TicketState:std::uint64_t { Free,Reserved,Completing,Submitted,Quarantined,Initializing };
    struct Ticket {
        // Generation and state share one CAS word, so a stale/replayed
        // completion can never claim a later reuse of the same fixed slot.
        std::atomic<std::uint64_t> phase{0};std::uint64_t generation=0;
        VkFence fence{};std::uint32_t mask=0;
        std::array<std::uint64_t,MaximumSlots> generations{};
        TicketState state()const noexcept{return static_cast<TicketState>(phase.load(std::memory_order_acquire)&7);}
        void state(TicketState value) noexcept{phase.store((generation<<3)|static_cast<std::uint64_t>(value),std::memory_order_release);}
        bool claim(std::uint64_t value) noexcept{
            auto expected=(value<<3)|static_cast<std::uint64_t>(TicketState::Reserved);
            return phase.compare_exchange_strong(expected,(value<<3)|static_cast<std::uint64_t>(TicketState::Completing),std::memory_order_acq_rel);
        }
        bool initialize(std::uint64_t value) noexcept {
            auto expected=phase.load(std::memory_order_acquire);
            return (expected&7)==static_cast<std::uint64_t>(TicketState::Free)&&
                phase.compare_exchange_strong(expected,(value<<3)|static_cast<std::uint64_t>(TicketState::Initializing),std::memory_order_acq_rel);
        }
    };
    mutable std::mutex gate;std::mutex cold;
    VkDevice device{};VkQueue queue{};VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout layout{};VkDescriptorPool descriptors{};VkSampler sampler{};VkCommandPool commands{};
    std::array<Slot,MaximumSlots> slots{};std::array<Ticket,MaximumSubmissionFences> tickets{};
    const std::uint64_t ownerGeneration=nextOwner();
    std::uint64_t serial=0,ticketSerial=0,uploadedCount=0;std::atomic<std::uint64_t> refusals{0};bool prepared=false;
    bool load(VkDevice d,PFN_vkGetDeviceProcAddr get) noexcept {
#define LOAD(n) n=reinterpret_cast<PFN_vk##n>(get(d,"vk" #n));if(!n)return false;
        GPU_FUNCTIONS(LOAD)
#undef LOAD
        return true;
    }
    std::uint32_t memoryType(std::uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred)const noexcept {
        for(unsigned pass=0;pass<2;++pass)for(std::uint32_t i=0;i<memory.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i)
            if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&required)==required&&
                (pass||(memory.memoryTypes[i].propertyFlags&preferred)==preferred))return i;
        return UINT32_MAX;
    }
    void releaseResource(Slot&s) noexcept {
        if(s.mapped)UnmapMemory(device,s.stagingMemory);
        if(s.view)DestroyImageView(device,s.view,nullptr);
        if(s.image)DestroyImage(device,s.image,nullptr);
        if(s.imageMemory)FreeMemory(device,s.imageMemory,nullptr);
        if(s.staging)DestroyBuffer(device,s.staging,nullptr);
        if(s.stagingMemory)FreeMemory(device,s.stagingMemory,nullptr);
        s.mapped=nullptr;s.view={};s.image={};s.imageMemory={};s.staging={};s.stagingMemory={};s.bytes=0;s.width=s.height=0;s.coherent=false;s.initializedShaderReadLayout=false;
    }
    void destroyIdle() noexcept {
        if(!device)return;
        for(auto&s:slots){releaseResource(s);if(s.uploaded)DestroyFence(device,s.uploaded,nullptr);s.uploaded={};}
        for(auto&t:tickets){if(t.fence)DestroyFence(device,t.fence,nullptr);t.fence={};}
        if(commands)DestroyCommandPool(device,commands,nullptr);
        if(descriptors)DestroyDescriptorPool(device,descriptors,nullptr);
        if(sampler)DestroySampler(device,sampler,nullptr);
        if(layout)DestroyDescriptorSetLayout(device,layout,nullptr);
        for(auto&s:slots){s.state=State::Free;s.frame={};s.generation=0;s.refs.store(0);s.quarantine.store(false);s.gpuRefs=0;s.set={};s.copy={};}
        for(auto&t:tickets){t.mask=0;t.generations.fill(0);t.state(TicketState::Free);}
        prepared=false;device={};commands={};descriptors={};sampler={};layout={};queue={};
    }
    bool leases(const Lease*ls,std::size_t n,std::uint32_t&mask,std::array<std::uint64_t,MaximumSlots>&gs)const noexcept {
        mask=0;gs.fill(0);if(!ls||!n||n>MaximumSlots)return false;
        for(std::size_t i=0;i<n;++i){const auto&l=ls[i];if(!l)return false;const auto&s=slots[l.slot];
            // Each supplied lease owns a recording reference throughout this
            // native submission. Check that epoch/ref first; a cold writer
            // cannot mutate any metadata while those references are retained.
            const auto refs=s.refs.load(std::memory_order_acquire);
            if((refs&Writer)||(refs>>16)!=l.generation||!(refs&RefMask))return false;
            // Copying means our own prior queue submit returned VK_SUCCESS.
            // Its transfer-to-fragment barrier also orders a first-use layout
            // transition before later native draws on this same ordered queue.
            // A CPU fence poll is storage-reuse proof, not sampling permission.
            const bool usable=s.state==State::Ready||(s.state==State::Copying);
            if(!usable||s.generation!=l.generation||s.set!=l.set||
                s.quarantine.load(std::memory_order_acquire))return false;
            mask|=1u<<l.slot;gs[l.slot]=l.generation;
        }return true;
    }
    Lease acquire(const FrameIdentity& frame,bool scheduled) noexcept {
        if(!prepared)return {};
        for(std::uint32_t index=0;index<slots.size();++index){auto& s=slots[index];auto refs=s.refs.load(std::memory_order_acquire);
            // Claim BEFORE reading non-atomic frame/set/generation metadata.
            // The only writer first CAS-claims Writer from zero references.
            for(unsigned attempt=0;attempt<8;++attempt){
                if((refs&Writer)||!(refs>>16)||(refs&RefMask)==RefMask)break;
                if(!s.refs.compare_exchange_strong(refs,refs+1,std::memory_order_acq_rel))continue;
                const auto state=s.state.load(std::memory_order_acquire);
                const bool usable=state==State::Ready||(scheduled&&(state==State::Pending||state==State::Submitting||state==State::Copying));
                if(usable&&!s.quarantine.load(std::memory_order_acquire)&&same(s.frame,frame))return {index,refs>>16,s.set};
                // This claim still owns one reference, so neither the writer
                // bit nor its generation can change until this decrement.
                s.refs.fetch_sub(1,std::memory_order_release);break;
            }
        }
        return {};
    }
    bool makeResource(Slot&s,std::uint32_t w,std::uint32_t h,stream_bridge::Bytes bytes) noexcept {
        const bool reuse=s.image&&s.view&&s.staging&&s.mapped&&s.width==w&&s.height==h&&s.bytes==bytes.size;
        if(!reuse)releaseResource(s);
        if(ResetCommandBuffer(s.copy,0)!=VK_SUCCESS||ResetFences(device,1,&s.uploaded)!=VK_SUCCESS)return false;
        if(!reuse){
        VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
        image.format=VK_FORMAT_R8_UNORM;image.extent={w,h,1};image.mipLevels=1;image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(CreateImage(device,&image,nullptr,&s.image)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};GetImageMemoryRequirements(device,s.image,&req);
        auto type=memoryType(req.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(type==UINT32_MAX)return false;
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        if(AllocateMemory(device,&allocation,nullptr,&s.imageMemory)!=VK_SUCCESS||BindImageMemory(device,s.image,s.imageMemory,0)!=VK_SUCCESS)return false;
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.image=s.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=VK_FORMAT_R8_UNORM;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(CreateImageView(device,&view,nullptr,&s.view)!=VK_SUCCESS)return false;
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;buffer.size=bytes.size;
        buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(CreateBuffer(device,&buffer,nullptr,&s.staging)!=VK_SUCCESS)return false;
        GetBufferMemoryRequirements(device,s.staging,&req);
        type=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(type==UINT32_MAX)return false;
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        if(AllocateMemory(device,&allocation,nullptr,&s.stagingMemory)!=VK_SUCCESS||BindBufferMemory(device,s.staging,s.stagingMemory,0)!=VK_SUCCESS||
            MapMemory(device,s.stagingMemory,0,VK_WHOLE_SIZE,0,&s.mapped)!=VK_SUCCESS)return false;
        s.bytes=bytes.size;s.width=w;s.height=h;
        s.coherent=(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        }
        std::memcpy(s.mapped,bytes.data,bytes.size);
        if(!s.coherent){
            VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=s.stagingMemory;range.size=VK_WHOLE_SIZE;
            if(FlushMappedMemoryRanges(device,1,&range)!=VK_SUCCESS)return false;
        }
        if(!reuse){
        VkDescriptorImageInfo imageInfo{sampler,s.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=s.set;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&imageInfo;
        UpdateDescriptorSets(device,1,&write,0,nullptr);
        }
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(BeginCommandBuffer(s.copy,&begin)!=VK_SUCCESS)return false;
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;barrier.oldLayout=reuse?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.srcAccessMask=reuse?VK_ACCESS_SHADER_READ_BIT:0;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=s.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(s.copy,reuse?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
        CmdCopyBufferToImage(s.copy,s.staging,s.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        CmdPipelineBarrier(s.copy,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        return EndCommandBuffer(s.copy)==VK_SUCCESS;
    }
};
Pool::Pool():impl_(new Impl){}
// Without an owner-proved idle teardown, raw Vulkan handles deliberately remain
// alive until device destruction. A destructor is not a GPU completion proof.
Pool::~Pool()=default;
bool Pool::prepare(VkDevice d,PFN_vkGetDeviceProcAddr get,const VkPhysicalDeviceMemoryProperties&m,std::uint32_t family) noexcept {
    auto&p=*impl_;std::lock_guard<std::mutex> cold(p.cold);std::lock_guard<std::mutex> lock(p.gate);
    if(p.device||!d||!get||!m.memoryTypeCount||m.memoryTypeCount>VK_MAX_MEMORY_TYPES||family==UINT32_MAX)return false;
    if(!p.load(d,get))return false;
    p.device=d;p.memory=m;p.GetDeviceQueue(d,family,0,&p.queue);
    if(!p.queue){p.destroyIdle();return false;}
    VkDescriptorSetLayoutBinding binding{};binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;layout.bindingCount=1;layout.pBindings=&binding;
    if(p.CreateDescriptorSetLayout(d,&layout,nullptr,&p.layout)!=VK_SUCCESS){p.destroyIdle();return false;}
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MaximumSlots};
    VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=MaximumSlots;pool.poolSizeCount=1;pool.pPoolSizes=&size;
    if(p.CreateDescriptorPool(d,&pool,nullptr,&p.descriptors)!=VK_SUCCESS){p.destroyIdle();return false;}
    VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;sampler.magFilter=VK_FILTER_LINEAR;sampler.minFilter=VK_FILTER_LINEAR;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(p.CreateSampler(d,&sampler,nullptr,&p.sampler)!=VK_SUCCESS){p.destroyIdle();return false;}
    VkCommandPoolCreateInfo command{};command.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;command.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;command.queueFamilyIndex=family;
    if(p.CreateCommandPool(d,&command,nullptr,&p.commands)!=VK_SUCCESS){p.destroyIdle();return false;}
    std::array<VkDescriptorSetLayout,MaximumSlots> layouts{};layouts.fill(p.layout);std::array<VkDescriptorSet,MaximumSlots> sets{};
    VkDescriptorSetAllocateInfo setsInfo{};setsInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;setsInfo.descriptorPool=p.descriptors;setsInfo.descriptorSetCount=MaximumSlots;setsInfo.pSetLayouts=layouts.data();
    std::array<VkCommandBuffer,MaximumSlots> copies{};VkCommandBufferAllocateInfo copiesInfo{};copiesInfo.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    copiesInfo.commandPool=p.commands;copiesInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;copiesInfo.commandBufferCount=MaximumSlots;
    if(p.AllocateDescriptorSets(d,&setsInfo,sets.data())!=VK_SUCCESS||p.AllocateCommandBuffers(d,&copiesInfo,copies.data())!=VK_SUCCESS){p.destroyIdle();return false;}
    VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for(std::size_t i=0;i<MaximumSlots;++i){p.slots[i].set=sets[i];p.slots[i].copy=copies[i];
        if(p.CreateFence(d,&fence,nullptr,&p.slots[i].uploaded)!=VK_SUCCESS){p.destroyIdle();return false;}}
    for(auto&t:p.tickets)if(p.CreateFence(d,&fence,nullptr,&t.fence)!=VK_SUCCESS){p.destroyIdle();return false;}
    p.prepared=true;return true;
}
VkDescriptorSetLayout Pool::descriptorLayout()const noexcept {return impl_->layout;}
bool Pool::upload(const FrameIdentity&f,std::uint32_t w,std::uint32_t h,stream_bridge::Bytes b) noexcept {
    auto&p=*impl_;if(!valid(f,w,h,b)){++p.refusals;return false;}std::lock_guard<std::mutex> cold(p.cold);
    Impl::Slot* selected=nullptr;Impl::Slot* duplicate=nullptr;
    {std::lock_guard<std::mutex> lock(p.gate);if(!p.prepared){++p.refusals;return false;}
        for(auto&s:p.slots)if(s.state!=Impl::State::Free&&same(s.frame,f)){duplicate=&s;break;}
    }
    // Only the cold owner changes storage. The potentially large duplicate
    // comparison deliberately runs outside the draw/queue state mutex.
    if(duplicate){
        bool match=duplicate->mapped&&duplicate->bytes==b.size&&std::memcmp(duplicate->mapped,b.data,b.size)==0;
        if(!match)++p.refusals;
        return match;
    }
    {std::lock_guard<std::mutex> lock(p.gate);std::uint32_t skipped=0;if(p.serial==GenerationLimit){++p.refusals;return false;}
        for(unsigned attempt=0;attempt<MaximumSlots;++attempt){selected=nullptr;unsigned index=0;
            for(unsigned n=0;n<p.slots.size();++n){auto& s=p.slots[n];if(!(skipped&(1u<<n))&&!s.quarantine.load(std::memory_order_acquire)&&!s.gpuRefs.load(std::memory_order_acquire)&&!(s.refs.load(std::memory_order_acquire)&(RefMask|Writer))&&
                (s.state==Impl::State::Free||s.state==Impl::State::Ready)&&(!selected||s.generation<selected->generation)){selected=&s;index=n;}}
            if(!selected)break;
            auto refs=selected->refs.load(std::memory_order_acquire);const auto original=refs;
            if((refs&(RefMask|Writer))||!selected->refs.compare_exchange_strong(refs,refs|Writer,std::memory_order_acq_rel)){skipped|=1u<<index;selected=nullptr;continue;}
            // A final recording release can race the first GPU-ref check.
            // Claiming its release sequence makes that prior increment visible.
            if(selected->gpuRefs.load(std::memory_order_acquire)){selected->refs.store(original,std::memory_order_release);skipped|=1u<<index;selected=nullptr;continue;}
            break;
        }
        if(!selected){++p.refusals;return false;}
        selected->state=Impl::State::Allocating;selected->frame=f;selected->generation=++p.serial;
    }
    bool ok=p.makeResource(*selected,w,h,b);
    if(!ok)p.releaseResource(*selected);
    {std::lock_guard<std::mutex> lock(p.gate);selected->state=ok?Impl::State::Pending:Impl::State::Free;
        selected->refs.store(selected->generation<<16,std::memory_order_release);
        if(ok)++p.uploadedCount;else ++p.refusals;}
    return ok;
}
void Pool::drainUploads(VkQueue q) noexcept {
    auto&p=*impl_;if(!p.prepared||q!=p.queue)return;
    for(auto&s:p.slots){auto expected=Impl::State::Pending;
        if(s.quarantine.load(std::memory_order_acquire)||!s.state.compare_exchange_strong(expected,Impl::State::Submitting,std::memory_order_acq_rel))continue;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&s.copy;
        if(p.QueueSubmit(q,1,&submit,s.uploaded)==VK_SUCCESS)s.state=Impl::State::Copying;
        else{s.quarantine.store(true,std::memory_order_release);s.state=Impl::State::Pending;++p.refusals;}
    }
}
void Pool::reapCold() noexcept {
    auto&p=*impl_;std::lock_guard<std::mutex> cold(p.cold);std::lock_guard<std::mutex> lock(p.gate);if(!p.prepared)return;
    for(auto&s:p.slots)if(s.state==Impl::State::Copying){auto r=p.GetFenceStatus(p.device,s.uploaded);
        if(r==VK_SUCCESS){s.initializedShaderReadLayout=true;s.state=Impl::State::Ready;}else if(r!=VK_NOT_READY){s.quarantine.store(true,std::memory_order_release);++p.refusals;}}
    for(auto&t:p.tickets)if(t.state()==Impl::TicketState::Submitted){auto r=p.GetFenceStatus(p.device,t.fence);
        if(r==VK_SUCCESS){for(std::size_t i=0;i<MaximumSlots;++i)if(t.mask&(1u<<i))--p.slots[i].gpuRefs;
            if(p.ResetFences(p.device,1,&t.fence)==VK_SUCCESS){t.mask=0;t.state(Impl::TicketState::Free);}
            else{t.state(Impl::TicketState::Quarantined);++p.refusals;}}
        else if(r!=VK_NOT_READY){t.state(Impl::TicketState::Quarantined);for(std::size_t i=0;i<MaximumSlots;++i)if(t.mask&(1u<<i))p.slots[i].quarantine.store(true,std::memory_order_release);++p.refusals;}
    }
}
Lease Pool::acquire(const FrameIdentity&f) noexcept {
    return impl_->acquire(f,false);
}
Lease Pool::acquireScheduled(const FrameIdentity&f) noexcept {
    return impl_->acquire(f,true);
}
void Pool::releaseRecorded(const Lease&l) noexcept {
    if(!l)return;
    auto&s=impl_->slots[l.slot];auto refs=s.refs.load(std::memory_order_acquire);
    for(unsigned tries=0;tries<8;++tries){if((refs>>16)!=l.generation||!(refs&RefMask))return;
        if(s.refs.compare_exchange_weak(refs,refs-1,std::memory_order_acq_rel))return;}
    s.quarantine.store(true,std::memory_order_release);++impl_->refusals;
}
Reservation Pool::prepareSubmit(VkQueue q,const Lease*ls,std::size_t n) noexcept {
    auto&p=*impl_;if(!n)return {};
    std::uint32_t mask=0;std::array<std::uint64_t,MaximumSlots> gs{};
    // No sampling submit has been enabled yet. Refusing this reservation
    // makes only this batch opaque; its recording references still retain
    // immutable storage until the native recording is completed/retired.
    if(!p.prepared||!p.ownerGeneration||q!=p.queue||!p.leases(ls,n,mask,gs)||p.ticketSerial==(std::numeric_limits<std::uint64_t>::max()>>3)){++p.refusals;return {};}
    // The original queue's external synchronization also owns ticketSerial.
    // Claim a Free slot before changing fields; the cold worker never reads
    // Initializing and only publishes Free after completed/reset retirement.
    for(std::uint32_t index=0;index<p.tickets.size();++index){auto&t=p.tickets[index];if(!t.initialize(p.ticketSerial+1))continue;
        t.generation=++p.ticketSerial;t.mask=mask;t.generations=gs;
        for(std::size_t i=0;i<MaximumSlots;++i)if(mask&(1u<<i))++p.slots[i].gpuRefs;
        t.state(Impl::TicketState::Reserved);return {index,t.generation,&p,p.ownerGeneration};}
    ++p.refusals;return {};
}
void Pool::submitted(const Reservation& reservation,VkQueue q,const Lease*ls,std::size_t n,VkResult original) noexcept {
    auto&p=*impl_;if(!reservation||reservation.owner_!=&p||reservation.ownerGeneration_!=p.ownerGeneration){++p.refusals;return;}
    auto&t=p.tickets[reservation.slot_];if(!t.claim(reservation.generation_)){++p.refusals;return;}
    // Only this exact generation's queue owner can touch these immutable facts.
    // The worker ignores Reserved/Completing and observes the release publish
    // only after the real tail submit, without a second contended mutex.
    const auto refuse=[&]{for(std::size_t i=0;i<MaximumSlots;++i)if(t.mask&(1u<<i))p.slots[i].quarantine.store(true,std::memory_order_release);t.state(Impl::TicketState::Quarantined);++p.refusals;};
    if(!p.prepared||q!=p.queue||!ls||!n||n>MaximumSlots){refuse();return;}
    std::uint32_t mask=0;std::array<std::uint64_t,MaximumSlots> gs{};
    for(std::size_t i=0;i<n;++i){if(!ls[i]){refuse();return;}mask|=1u<<ls[i].slot;gs[ls[i].slot]=ls[i].generation;}
    if(t.mask!=mask||t.generations!=gs){refuse();return;}
    if(original==VK_SUCCESS&&p.QueueSubmit(q,0,nullptr,t.fence)==VK_SUCCESS)t.state(Impl::TicketState::Submitted);
    else refuse();
}
Status Pool::statusCold()const noexcept {
    auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);Status out{};out.uploaded=p.uploadedCount;out.refused=p.refusals.load();
    for(const auto&s:p.slots){out.pending+=s.state==Impl::State::Pending;out.copying+=s.state==Impl::State::Copying;out.ready+=s.state==Impl::State::Ready;
        out.recorded+=static_cast<std::uint32_t>(s.refs.load()&RefMask);out.submitted+=s.gpuRefs;out.quarantined+=s.quarantine.load();}return out;
}
void Pool::destroyAfterIdle() noexcept {auto&p=*impl_;std::lock_guard<std::mutex> cold(p.cold);std::lock_guard<std::mutex> lock(p.gate);p.destroyIdle();}
void Pool::abandonAfterDeviceDestroy() noexcept {
    // The actual device is already gone; Vulkan calls are no longer legal.
    // Retain the nonthrowing object itself and discard only its raw handles.
    auto&p=*impl_;std::lock_guard<std::mutex> cold(p.cold);std::lock_guard<std::mutex> lock(p.gate);
    p.prepared=false;p.device={};p.queue={};p.layout={};p.descriptors={};p.sampler={};p.commands={};
    for(auto&s:p.slots){s.image={};s.imageMemory={};s.view={};s.staging={};s.stagingMemory={};s.mapped=nullptr;s.uploaded={};s.copy={};s.set={};s.initializedShaderReadLayout=false;
        s.state=Impl::State::Free;s.frame={};s.generation=0;s.refs.store(0);s.gpuRefs=0;s.quarantine.store(false);}
    for(auto&t:p.tickets){t.mask=0;t.generations.fill(0);t.state(Impl::TicketState::Free);}
    for(auto&t:p.tickets)t.fence={};
}
} // namespace stream_bridge_coverage_gpu
