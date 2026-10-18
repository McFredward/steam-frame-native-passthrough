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
constexpr std::uint64_t RefMask=65535,GenerationLimit=(std::numeric_limits<std::uint64_t>::max()>>16);
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
    enum class State { Free,Allocating,Pending,Copying,Ready };
    struct Slot {
        State state=State::Free;FrameIdentity frame{};std::uint64_t generation=0;
        std::atomic<std::uint64_t> refs{0};std::atomic<bool> quarantine{false};
        std::uint32_t gpuRefs=0;VkImage image{};VkDeviceMemory imageMemory{};
        VkImageView view{};VkBuffer staging{};VkDeviceMemory stagingMemory{};
        void* mapped=nullptr;std::size_t bytes=0;std::uint32_t width=0,height=0;bool coherent=false,initializedShaderReadLayout=false;VkDescriptorSet set{};
        VkCommandBuffer copy{};VkFence uploaded{};
    };
    enum class TicketState { Free,Reserved,Submitted,Quarantined };
    struct Ticket {
        TicketState state=TicketState::Free;VkFence fence{};std::uint32_t mask=0;
        std::array<std::uint64_t,MaximumSlots> generations{};
    };
    mutable std::mutex gate;std::mutex cold;
    VkDevice device{};VkQueue queue{};VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout layout{};VkDescriptorPool descriptors{};VkSampler sampler{};VkCommandPool commands{};
    std::array<Slot,MaximumSlots> slots{};std::array<Ticket,MaximumSubmissionFences> tickets{};
    std::uint64_t serial=0,uploadedCount=0;std::atomic<std::uint64_t> refusals{0};bool prepared=false;
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
        for(auto&t:tickets){t.state=TicketState::Free;t.mask=0;t.generations.fill(0);}
        prepared=false;device={};commands={};descriptors={};sampler={};layout={};queue={};
    }
    void quarantine(const Lease*ls,std::size_t n) noexcept {
        if(!ls||n>MaximumSlots){for(auto&s:slots)s.quarantine.store(true,std::memory_order_release);return;}
        for(std::size_t i=0;i<n;++i)if(ls[i].slot<MaximumSlots)slots[ls[i].slot].quarantine.store(true,std::memory_order_release);
    }
    bool leases(const Lease*ls,std::size_t n,std::uint32_t&mask,std::array<std::uint64_t,MaximumSlots>&gs)const noexcept {
        mask=0;gs.fill(0);if(!ls||!n||n>MaximumSlots)return false;
        for(std::size_t i=0;i<n;++i){const auto&l=ls[i];if(!l)return false;const auto&s=slots[l.slot];
            // Copying means our own prior queue submit returned VK_SUCCESS.
            // It is usable before CPU reap only on that same ordered queue,
            // and only when a previous completed upload initialized this image.
            const bool usable=s.state==State::Ready||(s.state==State::Copying&&s.initializedShaderReadLayout);
            if(!usable||s.generation!=l.generation||s.set!=l.set||
                s.quarantine.load(std::memory_order_acquire)||!(s.refs.load(std::memory_order_acquire)&RefMask))return false;
            mask|=1u<<l.slot;gs[l.slot]=l.generation;
        }return true;
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
    {std::lock_guard<std::mutex> lock(p.gate);
        for(auto&s:p.slots)if(!s.quarantine.load(std::memory_order_acquire)&&!s.gpuRefs&&!(s.refs.load(std::memory_order_acquire)&RefMask)&&
            (s.state==Impl::State::Free||s.state==Impl::State::Ready)&&(!selected||s.generation<selected->generation))selected=&s;
        if(!selected||p.serial==GenerationLimit){++p.refusals;return false;}
        selected->state=Impl::State::Allocating;selected->frame=f;selected->generation=++p.serial;
        selected->refs.store(selected->generation<<16,std::memory_order_release);
    }
    bool ok=p.makeResource(*selected,w,h,b);
    if(!ok)p.releaseResource(*selected);
    {std::lock_guard<std::mutex> lock(p.gate);selected->state=ok?Impl::State::Pending:Impl::State::Free;
        if(ok)++p.uploadedCount;else ++p.refusals;}
    return ok;
}
void Pool::drainUploads(VkQueue q) noexcept {
    auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock||!p.prepared||q!=p.queue)return;
    for(auto&s:p.slots)if(s.state==Impl::State::Pending&&!s.quarantine.load(std::memory_order_acquire)){
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&s.copy;
        if(p.QueueSubmit(q,1,&submit,s.uploaded)==VK_SUCCESS)s.state=Impl::State::Copying;
        else{s.quarantine.store(true,std::memory_order_release);++p.refusals;}
    }
}
void Pool::reapCold() noexcept {
    auto&p=*impl_;std::lock_guard<std::mutex> cold(p.cold);std::lock_guard<std::mutex> lock(p.gate);if(!p.prepared)return;
    for(auto&s:p.slots)if(s.state==Impl::State::Copying){auto r=p.GetFenceStatus(p.device,s.uploaded);
        if(r==VK_SUCCESS){s.initializedShaderReadLayout=true;s.state=Impl::State::Ready;}else if(r!=VK_NOT_READY){s.quarantine.store(true,std::memory_order_release);++p.refusals;}}
    for(auto&t:p.tickets)if(t.state==Impl::TicketState::Submitted){auto r=p.GetFenceStatus(p.device,t.fence);
        if(r==VK_SUCCESS){for(std::size_t i=0;i<MaximumSlots;++i)if(t.mask&(1u<<i))--p.slots[i].gpuRefs;
            if(p.ResetFences(p.device,1,&t.fence)==VK_SUCCESS){t.state=Impl::TicketState::Free;t.mask=0;}
            else{t.state=Impl::TicketState::Quarantined;++p.refusals;}}
        else if(r!=VK_NOT_READY){t.state=Impl::TicketState::Quarantined;for(std::size_t i=0;i<MaximumSlots;++i)if(t.mask&(1u<<i))p.slots[i].quarantine.store(true,std::memory_order_release);++p.refusals;}
    }
}
Lease Pool::acquire(const FrameIdentity&f) noexcept {
    auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock||!p.prepared)return {};
    for(std::uint32_t i=0;i<MaximumSlots;++i){auto&s=p.slots[i];if(s.state!=Impl::State::Ready||!same(s.frame,f)||s.quarantine.load(std::memory_order_acquire))continue;
        auto refs=s.refs.load(std::memory_order_acquire);if((refs&RefMask)==RefMask){++p.refusals;return {};}
        s.refs.fetch_add(1,std::memory_order_acq_rel);return {i,s.generation,s.set};
    }return {};
}
Lease Pool::acquireScheduled(const FrameIdentity&f) noexcept {
    auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock||!p.prepared)return {};
    for(std::uint32_t i=0;i<MaximumSlots;++i){auto&s=p.slots[i];
        const bool eligible=s.state==Impl::State::Ready||
            ((s.state==Impl::State::Pending||s.state==Impl::State::Copying)&&s.initializedShaderReadLayout);
        if(!eligible||!same(s.frame,f)||s.quarantine.load(std::memory_order_acquire))continue;
        auto refs=s.refs.load(std::memory_order_acquire);if((refs&RefMask)==RefMask){++p.refusals;return {};}
        s.refs.fetch_add(1,std::memory_order_acq_rel);return {i,s.generation,s.set};
    }return {};
}
void Pool::releaseRecorded(const Lease&l) noexcept {
    if(!l)return;
    auto&s=impl_->slots[l.slot];auto refs=s.refs.load(std::memory_order_acquire);
    for(unsigned tries=0;tries<8;++tries){if((refs>>16)!=l.generation||!(refs&RefMask))return;
        if(s.refs.compare_exchange_weak(refs,refs-1,std::memory_order_acq_rel))return;}
    s.quarantine.store(true,std::memory_order_release);++impl_->refusals;
}
bool Pool::prepareSubmit(VkQueue q,const Lease*ls,std::size_t n) noexcept {
    auto&p=*impl_;if(!n)return true;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    std::uint32_t mask=0;std::array<std::uint64_t,MaximumSlots> gs{};
    if(!lock||!p.prepared||q!=p.queue||!p.leases(ls,n,mask,gs)){p.quarantine(ls,n);++p.refusals;return false;}
    for(auto&t:p.tickets)if(t.state==Impl::TicketState::Free){t.state=Impl::TicketState::Reserved;t.mask=mask;t.generations=gs;
        for(std::size_t i=0;i<MaximumSlots;++i)if(mask&(1u<<i))++p.slots[i].gpuRefs;
        return true;}
    p.quarantine(ls,n);++p.refusals;return false;
}
void Pool::submitted(VkQueue q,const Lease*ls,std::size_t n,VkResult original) noexcept {
    auto&p=*impl_;if(!n)return;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    if(!lock||!p.prepared||q!=p.queue||!ls||n>MaximumSlots){p.quarantine(ls,n);++p.refusals;return;}
    std::uint32_t mask=0;std::array<std::uint64_t,MaximumSlots> gs{};
    for(std::size_t i=0;i<n;++i){if(!ls[i]){p.quarantine(ls,n);++p.refusals;return;}mask|=1u<<ls[i].slot;gs[ls[i].slot]=ls[i].generation;}
    for(auto&t:p.tickets)if(t.state==Impl::TicketState::Reserved&&t.mask==mask&&t.generations==gs){
        if(original==VK_SUCCESS&&p.QueueSubmit(q,0,nullptr,t.fence)==VK_SUCCESS)t.state=Impl::TicketState::Submitted;
        else{t.state=Impl::TicketState::Quarantined;p.quarantine(ls,n);++p.refusals;}return;
    }p.quarantine(ls,n);++p.refusals;
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
    for(auto&t:p.tickets){t.state=Impl::TicketState::Free;t.mask=0;t.generations.fill(0);}
    for(auto&t:p.tickets)t.fence={};
}
} // namespace stream_bridge_coverage_gpu
