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
class Renderer {
public:
    Renderer();~Renderer();
    Renderer(const Renderer&)=delete;
    bool prepareCold(VkDevice,VkPhysicalDeviceMemoryProperties,PFN_vkGetDeviceProcAddr,
        const VkDeviceCreateInfo&,std::uint32_t api,std::uint32_t graphicsFamily) noexcept;
    // Existing bounded cold definition recorder, not a GPU/frame authority.
    frame_pixel_ssbo_journal::Journal& definitions() noexcept;
    void graphicsCreated(VkPipelineCache,const VkGraphicsPipelineCreateInfo&,
        const VkAllocationCallbacks*,VkPipeline) noexcept;
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
    void stopCold() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge_renderer
