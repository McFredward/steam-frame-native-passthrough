#pragma once

// Archived opt-in diagnostic source only. The installed product uses the
// paired exact-coverage bridge; the old release build flag is retired.

#include "native_presenter_pipeline.h"
#include <atomic>

namespace frame_native_presenter_receiver {
namespace native = frame_native_presenter;
constexpr const char* EnableMagic = "GHVR_SYNTHETIC_PRESENTER/1\n";
constexpr const char* StatusFile = "synthetic-presenter-status.json";
constexpr const char* SceneEnableMagic = "GHVR_SCENE_PRESENTER/1\n";
constexpr const char* SceneStatusFile = "scene-presenter-status.json";
constexpr unsigned MaximumStatusUpdates = 32;
enum class SourceReason : std::uint8_t { Disabled, Directory, Marker, Module, Pin, Ready };
const char* sourceReasonName(SourceReason) noexcept;
const char* primitiveReasonName(native::Reason) noexcept;
const char* graphicsGateName(native::GraphicsGate) noexcept;

// Prepare exactly once, on a cold path. The copied artifact then stays immutable
// for this process session. Removing the opt-in is not a live pipeline switch.
class Source {
public:
    bool prepareCold(int ownedDirectoryFd) noexcept;
    bool ready() const noexcept { return reason_ == SourceReason::Ready; }
    SourceReason reason() const noexcept { return reason_; }
    const native::Fragment& fragment() const noexcept { return fragment_; }
    native::Profile profile() const noexcept { return profile_; }
    const char* statusFile() const noexcept {
        return profile_ == native::Profile::SceneContribution ? SceneStatusFile : StatusFile;
    }
private:
    bool attempted_ = false;
    SourceReason reason_ = SourceReason::Disabled;
    native::Fragment fragment_;
    native::Profile profile_ = native::Profile::Synthetic;
};

struct Retirement {
    void* owner = nullptr;
    void (*beforeDestroy)(void*, VkDevice, VkPipeline) noexcept = nullptr;
};
// This function is only for the same synchronous successful ORIGINAL pipeline
// creation completion. It is not an API for a historical recipe or exposed
// pipeline. The callback retires only the exact original device+handle before
// the downstream destroy; partial new handles do not receive that callback.
native::Result complete(VkDevice, VkPipelineCache, std::uint32_t negotiatedApi,
                        VkResult originalResult, const VkGraphicsPipelineCreateInfo&,
                        const VkAllocationCallbacks*, VkPipeline&,
                        const frame_pixel_ssbo_clone::Recipe&, const Source&,
                        const native::Dispatch&, Retirement) noexcept;

struct Snapshot {
    std::uint64_t revision = 0, pipelineAttempts = 0, pipelineReplacements = 0;
    std::uint64_t observedDevice = 0;
    std::uint32_t observedApi = 0, activeDevices = 0;
    native::Reason primitiveReason = native::Reason::None;
    bool definitionHistoryFailed = false;
    native::GraphicsGate graphicsGate = native::GraphicsGate::None;
    VkFormat attachmentFormat = VK_FORMAT_UNDEFINED;
    VkDynamicState unsupportedDynamic = VK_DYNAMIC_STATE_MAX_ENUM;
};
// Diagnostic atomics, not a coherent native source/frame receipt. All writers
// are cold device/pipeline lifecycle callbacks; no draw/submit observation.
class Counters {
public:
    void deviceCreated(VkDevice, std::uint32_t) noexcept;
    void deviceDestroyed() noexcept;
    void attempted(const native::Result&) noexcept;
    void historyFailed() noexcept;
    Snapshot snapshot() const noexcept;
private:
    std::atomic<std::uint64_t> revision_{1}, attempts_{0}, replacements_{0}, device_{0};
    std::atomic<std::uint32_t> api_{0}, devices_{0};
    std::atomic<native::Reason> reason_{native::Reason::None};
    std::atomic<bool> failed_{false};
    std::atomic<native::GraphicsGate> graphicsGate_{native::GraphicsGate::None};
    std::atomic<VkFormat> attachmentFormat_{VK_FORMAT_UNDEFINED};
    std::atomic<VkDynamicState> unsupportedDynamic_{VK_DYNAMIC_STATE_MAX_ENUM};
};
// Single prepared background writer only. Uses fixed private filenames, <=1KiB
// atomic replacements. Caller enforces the 32-update budget and joins the
// writer; this never belongs in a Vulkan creation/recording/submit callback.
bool writeStatusCold(int ownedDirectoryFd, const char* captureId, const Source&,
                     const Snapshot&, unsigned update, bool budgetExhausted) noexcept;
} // namespace frame_native_presenter_receiver
