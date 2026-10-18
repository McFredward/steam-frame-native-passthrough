#pragma once
#include <vulkan/vulkan.h>
#include "../bridge/stream_bridge_frame.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace stream_bridge_coverage_gpu {
constexpr std::uint32_t MaximumSlots=8;
constexpr std::uint32_t MaximumSubmissionFences=64;
struct FrameIdentity {
    stream_bridge::Nonce binding{};
    stream_bridge::FrameDescriptor descriptor{};
};
struct Lease {
    std::uint32_t slot=MaximumSlots;
    std::uint64_t generation=0;
    VkDescriptorSet set=VK_NULL_HANDLE;
    explicit operator bool()const noexcept{return slot<MaximumSlots&&generation&&set!=VK_NULL_HANDLE;}
};
struct Status {
    std::uint32_t pending=0,copying=0,ready=0,recorded=0,submitted=0,quarantined=0;
    std::uint64_t uploaded=0,refused=0;
};
class Pool {
public:
    Pool();~Pool();
    Pool(const Pool&)=delete;Pool& operator=(const Pool&)=delete;
    // Cold only. Caller proves this requested nonprotected graphics family and
    // supplies next-layer functions/memory facts for this same actual device.
    bool prepare(VkDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&,
        std::uint32_t queueFamily) noexcept;
    VkDescriptorSetLayout descriptorLayout()const noexcept;
    // Worker only. Copies owned R8 data; does not submit to a native queue.
    // Identity/extent must already originate in authenticated native association.
    bool upload(const FrameIdentity&,std::uint32_t width,std::uint32_t height,
        stream_bridge::Bytes rawR8) noexcept;
    // Called under the admitted ORIGINAL queue's external synchronization.
    // Submit pending copies while native draws remain on the original fallback.
    void drainUploads(VkQueue) noexcept;
    // Worker only: nonblocking fence polling makes completed uploads acquirable.
    void reapCold() noexcept;
    // Draw/recording: try-lock only, no allocation/wait/API. Ready immutable data
    // only. One successful acquire owns one recording reference until release.
    Lease acquire(const FrameIdentity&) noexcept;
    // Renderer-only recording preview. Pending/Copying is allowed solely for
    // an already initialized same-extent image. It is NOT submit permission:
    // drainUploads must enqueue the new copy on the exact admitted queue, then
    // prepareSubmit must succeed before the caller enables mask coverage.
    Lease acquireScheduled(const FrameIdentity&) noexcept;
    void releaseRecorded(const Lease&) noexcept;
    // Caller holds its admitted queue owner across original submission. Failure
    // to reserve/submit a tail fence quarantines immutable data; never free or
    // reuse resources sampled by an already recorded/submitted native command.
    bool prepareSubmit(VkQueue,const Lease*,std::size_t) noexcept;
    void submitted(VkQueue,const Lease*,std::size_t,VkResult originalResult) noexcept;
    Status statusCold()const noexcept;
    // No wait inside this helper. Owner stops worker and proves actual device
    // idle BEFORE destroying. A destroyed device may instead be abandoned.
    void destroyAfterIdle() noexcept;
    void abandonAfterDeviceDestroy() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge_coverage_gpu
