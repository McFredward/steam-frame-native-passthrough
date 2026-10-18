#pragma once
#include "stream_bridge_native_decode.h"
#include <memory>
namespace stream_bridge_native {
using DeviceHandle=std::uintptr_t;
using ImageHandle=std::uint64_t;
using MemoryHandle=std::uint64_t;
struct ImageDescription {std::uint32_t width=0,height=0,format=0;bool dmaBuf=false;};
struct ImageLease {
    DeviceHandle device=0;ImageHandle image=0;MemoryHandle memory=0;
    std::uint64_t imageGeneration=0,memoryGeneration=0,deviceIdentity=0,inodeIdentity=0;
    ImageDescription description{};CaptureObservation capture{};
};
// Tests may supply the already audited source-preparation predicate. Production
// uses the exact prepared main image + actual retained export return-site gate.
using ExportPreparation = bool(*)(std::uintptr_t caller,void*) noexcept;
class NativeImages {
public:
    static constexpr std::size_t Capacity=64;
    NativeImages();
    NativeImages(ExportPreparation,void*);~NativeImages();
    NativeImages(const NativeImages&)=delete;
    NativeImages& operator=(const NativeImages&)=delete;
    // Scalar success observations of the ORIGINAL public Vulkan calls. No
    // wrapper/native private-object field is accepted as a VkImage identity.
    bool created(DeviceHandle,ImageHandle,const ImageDescription&) noexcept;
    bool allocated(DeviceHandle,MemoryHandle,bool exportDmaBuf) noexcept;
    bool bound(DeviceHandle,ImageHandle,MemoryHandle,std::uint64_t offset) noexcept;
    void destroyed(DeviceHandle,ImageHandle) noexcept;
    void freed(DeviceHandle,MemoryHandle) noexcept;
    // Resource preparation ONLY, synchronously after successful DMA_BUF export
    // and before returning its fd to native. Duplicates and stats that exact fd;
    // a worker-delayed raw fd could already refer to an unrelated allocation.
    // handleType must equal VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT.
    bool exported(DeviceHandle,MemoryHandle,int fd,std::uint32_t handleType,
        std::uintptr_t actualCaller) noexcept;
    // Worker only: fstat the actual capture fd and compare retained allocation
    // identity, unique public memory/image binding and exact capture lease.
    bool join(const CaptureObservation&,ImageLease&) noexcept;
    // No I/O or waits: must be combined with pinnedCaptureLeaseCurrent() at
    // each selected draw. This alone cannot know that native requeued a buffer.
    bool current(const ImageLease&) const noexcept;
    void reset() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
