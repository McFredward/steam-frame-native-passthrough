#pragma once
#include <vulkan/vulkan.h>
#include "../bridge/stream_bridge_frame.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace stream_bridge_coverage_gpu {
constexpr std::uint32_t MaximumSlots=32;
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
class Reservation {
    friend class Pool;
    std::uint32_t slot_=MaximumSubmissionFences;
    std::uint64_t generation_=0;
    const void* owner_=nullptr;std::uint64_t ownerGeneration_=0;
    Reservation(std::uint32_t slot,std::uint64_t generation,const void* owner,std::uint64_t ownerGeneration) noexcept
        :slot_(slot),generation_(generation),owner_(owner),ownerGeneration_(ownerGeneration){}
public:
    Reservation()=default;
    explicit operator bool()const noexcept{return slot_<MaximumSubmissionFences&&generation_&&owner_&&ownerGeneration_;}
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
    // Enqueue pending copies before sampling on this exact ordered queue.
    void drainUploads(VkQueue) noexcept;
    // Worker only: nonblocking fence polling makes completed uploads acquirable.
    void reapCold() noexcept;
    // Draw/recording: bounded reference CAS, no mutex/allocation/wait/API.
    // Claim ownership before reading immutable metadata; a cold writer claims
    // zero references exclusively before replacing it. Ready immutable data
    // only. One successful acquire owns one recording reference until release.
    Lease acquire(const FrameIdentity&) noexcept;
    // Renderer-only recording preview. Pending/Copying is allowed solely for
    // the same-extent image, including its first upload. It is NOT submit permission:
    // drainUploads must enqueue the new copy on the exact admitted queue, then
    // prepareSubmit must succeed before the caller enables mask coverage.
    Lease acquireScheduled(const FrameIdentity&) noexcept;
    void releaseRecorded(const Lease&) noexcept;
    // Caller holds its admitted queue owner across original submission. A
    // refused reservation must keep this batch opaque; recording references
    // still retain storage. Call submitted ONLY after a successful reservation.
    // Post-submit ambiguity/tail failure quarantines data that may be in flight.
    // The generation-bound reservation must be completed exactly once by this
    // same externally synchronized queue owner. Neither queue operation takes
    // the cold worker's mutex; fixed ticket fields publish only after init.
    Reservation prepareSubmit(VkQueue,const Lease*,std::size_t) noexcept;
    void submitted(const Reservation&,VkQueue,const Lease*,std::size_t,VkResult originalResult) noexcept;
    Status statusCold()const noexcept;
    // No wait inside this helper. Owner stops worker and proves actual device
    // idle BEFORE destroying. A destroyed device may instead be abandoned.
    void destroyAfterIdle() noexcept;
    void abandonAfterDeviceDestroy() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge_coverage_gpu
