#pragma once
#include "../receiver/stream_bridge_local.h"
#include "../receiver/stream_bridge_native_images.h"
#include "../bridge/stream_bridge_frame.h"
#include <memory>
#include <atomic>
namespace stream_bridge_renderer {
enum class ProcessKind {Off,Vrlink,Compositor};
struct LocalPicture {
    stream_bridge_local::Message metadata{};
    std::shared_ptr<stream_bridge_local::LeaseReader> leases;
    std::shared_ptr<std::atomic<bool>> connection;
};
struct ProcessConsumer {
    void* owner=nullptr;
    void (*coverage)(void*,const stream_bridge::CertifiedAccessUnit&) noexcept=nullptr;
    void (*picture)(void*,const LocalPicture&) noexcept=nullptr;
    void (*tick)(void*) noexcept=nullptr;
    bool (*admitted)(void*) noexcept=nullptr;
    // Exact pre-draw retention; never depends on a future native image import.
    bool (*published)(void*,const LocalPicture&) noexcept=nullptr;
};
bool prepareProcessCold() noexcept;
bool enabled() noexcept;
// Renderer-only readiness observation. No settings/library/process or I/O work.
bool cameraAlphaReady() noexcept;
std::uint64_t cameraAlphaEpoch() noexcept;
ProcessKind processKind() noexcept;
stream_bridge_native::NativeImages* sourceImages() noexcept;
void sourceExportedFd(int) noexcept;
bool registerConsumerCold(ProcessConsumer) noexcept;
void unregisterConsumerCold(void*) noexcept;
bool readCoverageFragmentCold(std::vector<std::uint32_t>&) noexcept;
} // namespace stream_bridge_renderer
