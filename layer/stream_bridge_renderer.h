#pragma once
#include <vulkan/vulkan.h>
#include "pixel_ssbo_definition_journal.h"
#include <memory>

namespace stream_bridge_renderer {
// Called once during the loader negotiation, never from a draw or queue callback.
// Validates the installed, private receiver configuration before native hooks.
bool prepareProcessCold() noexcept;
bool enabled() noexcept;
struct Draw {
    VkPipeline original{},replacement{};
    VkPipelineLayout layout{};
    VkDescriptorSet coverage{},control{};
    bool ready=false;
};
struct MemoryPreparation {int duplicate=-1;std::uint64_t device=0,inode=0;};
enum class DrawRefusal:std::uint8_t {Unavailable,Busy,Command,Epoch,Pipeline,Ineligible,Descriptor,Image,ExactPicture,ReadyMask,Capacity,Count};
struct RendererStats {
    std::uint64_t metadata=0,maskNone=0,maskDecoded=0,maskRejected=0,uploadAccepted=0,uploadRefused=0,pictures=0;
    std::uint64_t begins=0,draws=0,selected=0,guardAttempts=0,guardAccepted=0,guardRefused=0,opaqueSubmits=0;
    std::array<std::uint64_t,static_cast<std::size_t>(DrawRefusal::Count)> drawRefusals{};
    std::uint32_t pending=0,copying=0,ready=0,quarantined=0,pipelines=0;
    std::uint64_t poolUploaded=0,poolRefused=0;
    bool prepared=false,lost=false;
};
class Renderer {
public:
    Renderer();~Renderer();
    Renderer(const Renderer&)=delete;
    bool prepareCold(VkDevice,VkPhysicalDeviceMemoryProperties,PFN_vkGetDeviceProcAddr,
        const VkDeviceCreateInfo&,std::uint32_t api,std::uint32_t graphicsFamily) noexcept;
    // Existing bounded cold definition recorder, not a GPU/frame authority.
    frame_pixel_ssbo_journal::Journal& definitions() noexcept;
    // Successful original cold shader creation; count1 admission requires the
    // exact original fragment's independently verified constant-zero profile.
    void shaderCreated(VkShaderModule,const VkShaderModuleCreateInfo&) noexcept;
    void graphicsCreated(VkPipelineCache,const VkGraphicsPipelineCreateInfo&,
        const VkAllocationCallbacks*,VkPipeline&) noexcept;
    void pipelineDestroyed(VkPipeline) noexcept;
    void imageCreated(VkImage,const VkImageCreateInfo&) noexcept;
    void imageDestroyed(VkImage) noexcept;
    MemoryPreparation memoryPreparing(const VkMemoryAllocateInfo&) noexcept;
    void memoryAllocated(VkDeviceMemory,const VkMemoryAllocateInfo&,MemoryPreparation&,VkResult) noexcept;
    void memoryFreed(VkDeviceMemory) noexcept;
    void imageBound(VkImage,VkDeviceMemory,VkDeviceSize) noexcept;
    void memoryExported(VkDeviceMemory,int,VkExternalMemoryHandleTypeFlagBits,
        std::uintptr_t actualCaller) noexcept;
    void viewCreated(VkImageView,const VkImageViewCreateInfo&) noexcept;
    void viewDestroyed(VkImageView) noexcept;
    void descriptorWrites(std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*) noexcept;
    void descriptorRetired(VkDescriptorSet) noexcept;
    void descriptorsInvalidated() noexcept;
    void queueObserved(VkQueue,std::uint32_t family) noexcept;
    void commandAllocated(VkCommandBuffer,VkCommandBufferLevel,std::uint32_t queueFamily) noexcept;
    void commandRetired(VkCommandBuffer) noexcept;
    void commandReset(VkCommandBuffer) noexcept;
    void trackingLost() noexcept;
    void commandBegin(VkCommandBuffer,const VkCommandBufferBeginInfo*) noexcept;
    void pipelineBound(VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
    void setsBound(VkCommandBuffer,VkPipelineBindPoint,std::uint32_t,std::uint32_t,
        const VkDescriptorSet*,std::uint32_t dynamicCount) noexcept;
    void commandUnknown(VkCommandBuffer) noexcept;
    Draw beforeDraw(VkCommandBuffer) noexcept;
    // The native queue owner calls these around its ORIGINAL submission. They
    // never wait; unsupported shape/route must keep ordinary native rendering.
    VkResult submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence) noexcept;
    VkResult submit2(VkQueue,std::uint32_t,const VkSubmitInfo2*,VkFence,bool khr=false) noexcept;
    // Cold diagnostics only. Counts are observations, never image authority;
    // a queued upload or recorded guard is not a successful native submission.
    RendererStats statsCold()const noexcept;
    void stopCold() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge_renderer
