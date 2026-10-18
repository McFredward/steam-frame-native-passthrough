#pragma once
// Isolated API fake for executing the actual layer wrapper. A separate build
// checks the production wrapper against the real renderer header.
#include <vulkan/vulkan.h>
#include <array>
#include <new>
namespace wrapper_fixture {
struct State {
    bool enabled=true,throwConstructor=false,throwJournal=false;
    unsigned prepared=0,stopped=0,allocated=0,retired=0,resets=0,begins=0,unknown=0,lost=0,journal=0;
    VkCommandBuffer lastCommand{};std::uint32_t family=0;VkCommandBufferLevel level{};
};
inline State state;
}
namespace stream_bridge_renderer {
inline bool prepareProcessCold() noexcept{return true;}
inline bool enabled() noexcept{return wrapper_fixture::state.enabled;}
struct Draw {VkPipeline original{},replacement{};VkPipelineLayout layout{};VkDescriptorSet coverage{},control{};bool ready=false;};
inline Draw fixtureDraw;
struct MemoryPreparation {int duplicate=-1;std::uint64_t device=0,inode=0;};
struct Journal {
    template<class... T>void record(T&&...){++wrapper_fixture::state.journal;if(wrapper_fixture::state.throwJournal)throw std::bad_alloc();}
    template<class... T>void createShader(T&&... t){record(t...);}template<class... T>void destroyShader(T&&... t){record(t...);}
    template<class... T>void createSet(T&&... t){record(t...);}template<class... T>void destroySet(T&&... t){record(t...);}
    template<class... T>void createLayout(T&&... t){record(t...);}template<class... T>void destroyLayout(T&&... t){record(t...);}
    template<class... T>void createRenderPass(T&&... t){record(t...);}template<class... T>void createRenderPass2(T&&... t){record(t...);}
    template<class... T>void destroyRenderPass(T&&... t){record(t...);}
};
class Renderer {
    Journal journal;VkDevice device{};PFN_vkGetDeviceProcAddr gdpa{};bool valid=true;
    std::array<VkCommandBuffer,128> commands{};
    bool known(VkCommandBuffer cb){for(const auto c:commands)if(c==cb)return true;return false;}
public:
    Renderer(){if(wrapper_fixture::state.throwConstructor)throw std::bad_alloc();}
    bool prepareCold(VkDevice d,VkPhysicalDeviceMemoryProperties,PFN_vkGetDeviceProcAddr get,const VkDeviceCreateInfo&,std::uint32_t,std::uint32_t) noexcept {device=d;gdpa=get;++wrapper_fixture::state.prepared;return true;}
    Journal& definitions() noexcept{return journal;}
    void stopCold() noexcept{++wrapper_fixture::state.stopped;}
    void trackingLost() noexcept{valid=false;++wrapper_fixture::state.lost;}
    void commandAllocated(VkCommandBuffer cb,VkCommandBufferLevel level,std::uint32_t family) noexcept {
        auto& s=wrapper_fixture::state;++s.allocated;s.lastCommand=cb;s.level=level;s.family=family;
        for(auto& c:commands)if(!c){c=cb;return;}
    }
    void commandRetired(VkCommandBuffer cb) noexcept {++wrapper_fixture::state.retired;for(auto& c:commands)if(c==cb)c={};}
    void commandReset(VkCommandBuffer cb) noexcept {if(known(cb)){++wrapper_fixture::state.resets;wrapper_fixture::state.lastCommand=cb;}}
    void commandBegin(VkCommandBuffer cb,const VkCommandBufferBeginInfo*) noexcept {if(known(cb))++wrapper_fixture::state.begins;}
    void commandUnknown(VkCommandBuffer) noexcept {++wrapper_fixture::state.unknown;}
    Draw beforeDraw(VkCommandBuffer cb) noexcept{return valid&&known(cb)?fixtureDraw:Draw{};}
    VkResult submit(VkQueue q,std::uint32_t n,const VkSubmitInfo* s,VkFence f) noexcept {return reinterpret_cast<PFN_vkQueueSubmit>(gdpa(device,"vkQueueSubmit"))(q,n,s,f);}
    VkResult submit2(VkQueue q,std::uint32_t n,const VkSubmitInfo2* s,VkFence f,bool khr) noexcept {return reinterpret_cast<PFN_vkQueueSubmit2>(gdpa(device,khr?"vkQueueSubmit2KHR":"vkQueueSubmit2"))(q,n,s,f);}
#define NOOP(name,signature) void name signature noexcept {}
    NOOP(graphicsCreated,(VkPipelineCache,const VkGraphicsPipelineCreateInfo&,const VkAllocationCallbacks*,VkPipeline))
    NOOP(pipelineDestroyed,(VkPipeline)) NOOP(imageCreated,(VkImage,const VkImageCreateInfo&)) NOOP(imageDestroyed,(VkImage))
    MemoryPreparation memoryPreparing(const VkMemoryAllocateInfo&) noexcept{return {};}
    NOOP(memoryAllocated,(VkDeviceMemory,const VkMemoryAllocateInfo&,MemoryPreparation&,VkResult)) NOOP(memoryFreed,(VkDeviceMemory))
    NOOP(imageBound,(VkImage,VkDeviceMemory,VkDeviceSize)) NOOP(memoryExported,(VkDeviceMemory,int,VkExternalMemoryHandleTypeFlagBits,std::uintptr_t))
    NOOP(viewCreated,(VkImageView,const VkImageViewCreateInfo&)) NOOP(viewDestroyed,(VkImageView))
    NOOP(descriptorWrites,(std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*))
    NOOP(descriptorRetired,(VkDescriptorSet)) NOOP(descriptorsInvalidated,()) NOOP(queueObserved,(VkQueue,std::uint32_t))
    NOOP(pipelineBound,(VkCommandBuffer,VkPipelineBindPoint,VkPipeline))
    NOOP(setsBound,(VkCommandBuffer,VkPipelineBindPoint,std::uint32_t,std::uint32_t,const VkDescriptorSet*,std::uint32_t))
#undef NOOP
};
}
