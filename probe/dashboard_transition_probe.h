#pragma once

// Uninstalled diagnostic only. No native image, descriptor or feature mutation.
#include "../layer/pixel_ssbo_clone_contract.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace dashboard_transition_probe {
constexpr std::size_t BufferBytes = 1600;
constexpr std::size_t Slots = 6;
constexpr std::size_t MaximumPipelines = 8;
constexpr std::size_t MaximumCommands = 512;
constexpr std::size_t GenericRouteBegin = MaximumPipelines;
constexpr const char* ShaderSha = "04846d731cedfc6f1a98254d2239b0b90f75225567aeac2b868747415766222a";
constexpr std::size_t ShaderBytes = 62908;
constexpr std::size_t PipelineTagWord = 1403;
constexpr std::uint32_t TagTemplate = 0x44545000;

enum class Refusal : unsigned {
    None, Disabled, Feature, QueueProfile, Dispatch, Artifact, Resource,
    PipelineCapacity, CommandCapacity, Recording, Submit, Queue, DeviceLost,
    Mutation, Collection, PhaseOrder
};

// Cold public API facts, copied before vkCreateDevice's borrowed inputs expire.
// UINT32_MAX means unavailable, never an invented queue family0.
struct QueueObservation {
    bool detailed=false, pfnPresent=false, secondQueryCalled=false;
    std::uint32_t firstCount=UINT32_MAX, secondCount=UINT32_MAX, rawFamily=UINT32_MAX;
    VkQueueFlags flags=0;
};
QueueObservation observeQueueFamily(PFN_vkGetPhysicalDeviceQueueFamilyProperties,
                                   VkPhysicalDevice,std::uint32_t rawFamily) noexcept;
enum QueueFailure : std::uint32_t {
    QueueCreateCount=1u<<0, QueueMissing=1u<<1, QueueType=1u<<2, QueueFlags=1u<<3,
    QueueCount=1u<<4, QueuePriorityMissing=1u<<5, QueuePriorityValue=1u<<6,
    QueueFamily=1u<<7, QueueNextType=1u<<8, QueueNextChain=1u<<9,
    QueueGlobalPriority=1u<<10, QueueQueryMissing=1u<<11,
    QueueQueryCount=1u<<12, QueueQueryChanged=1u<<13,
    QueueQueryRange=1u<<14, QueueNotGraphics=1u<<15, QueueQueryFlags=1u<<16
};
struct QueueProfile {
    QueueObservation query{};
    std::uint32_t failures=0, type=UINT32_MAX, flags=0, count=0, rawFamily=UINT32_MAX;
    std::uint32_t priorityBits=0, nextType=UINT32_MAX;
    std::int32_t globalPriority=INT32_MIN;
    bool available=false, priorityObserved=false, nextPresent=false,
         nextHasNext=false, globalPriorityObserved=false;
};

// Each stage is spent once. Reopening a session, resetting a command pool or
// re-reading a marker cannot rearm old slots. The final stage disables GPU writes.
class PhaseSequence {
public:
    bool request(unsigned value) noexcept {
        const auto before = requested.load(std::memory_order_acquire);
        if (value == before) return true;
        if ((before == 3 && !started && value == 0) ||
            (started && before < 3 && value == before + 1)) {
            started = true; requested.store(value, std::memory_order_release); return true;
        }
        return false;
    }
    std::atomic<unsigned> requested{3};
private:
    bool started = false; // One cold/background writer.
};

class Probe {
public:
    Probe() = default;
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe(); // Joins CPU writer only; Vulkan release is explicit and quiescent.
    void prepare(VkDevice, const VkDeviceCreateInfo&, PFN_vkGetDeviceProcAddr,
                 const VkPhysicalDeviceMemoryProperties&, int ownedDirectoryFd, VkQueueFlags observedQueueFlags,
                 const QueueObservation* detailedObservation=nullptr) noexcept;
    void pipeline(VkPipeline normal, VkPipelineCache, const VkGraphicsPipelineCreateInfo&,
                  const frame_pixel_ssbo_clone::Recipe&) noexcept;
    void pipelineRetiring(VkPipeline) noexcept;
    void shaderObserved(VkShaderModule, const VkShaderModuleCreateInfo&) noexcept;
    void pipelineObserved(VkPipeline, const VkGraphicsPipelineCreateInfo&, unsigned nativeReason = UINT32_MAX) noexcept;
    void queueRetrieved(std::uint32_t family, std::uint32_t index, VkQueue, bool ordinary = true) noexcept;
    void commandsAllocated(const VkCommandBufferAllocateInfo&, const VkCommandBuffer*, VkResult) noexcept;
    void commandsFreed(VkCommandPool, std::uint32_t, const VkCommandBuffer*) noexcept;
    void poolRetired(VkCommandPool, bool destroyed = false) noexcept;
    void begin(VkCommandBuffer, const VkCommandBufferBeginInfo&, VkResult) noexcept;
    void end(VkCommandBuffer, VkResult) noexcept;
    void reset(VkCommandBuffer, VkResult) noexcept;
    void bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) noexcept;
    void nativeDescriptorBinding(VkCommandBuffer, VkPipelineBindPoint, std::uint32_t firstSet, std::uint32_t count) noexcept;
    void unknown(VkCommandBuffer) noexcept;
    bool beforeDraw(VkCommandBuffer) noexcept;
    void afterDraw(VkCommandBuffer) noexcept;
    void beforeSubmit(VkQueue, bool ordinary, std::uint32_t count = 0, const VkSubmitInfo* = nullptr) noexcept;
    void release(VkResult actualDeviceIdle) noexcept;
    Refusal refusal() const noexcept { return refusal_.load(std::memory_order_acquire); }
    bool prepared() const noexcept { return ready_; }
    friend struct TestAccess;
private:
    struct Pipeline { VkPipeline normal{}, instrumented{}; VkPipelineLayout layout{}; std::uint32_t tag{}; bool retired{}; };
    struct Command { VkCommandBuffer value{}; VkCommandPool pool{}; VkPipeline bound{}; std::uint64_t usedRoutes{}; bool unknownRoute{}, primary{}, recording{}, eligible{}, swapped{}; };
    struct ShaderIdentity { std::uint64_t handle{}; std::uint32_t bytes{}; std::array<std::uint8_t,32> sha{}; };
    struct Route { std::uint64_t handle{}; std::uint32_t tag{}, nativeReason{}, retired{}, reserved{}; std::array<std::uint8_t,32> vs{}, ps{}; std::array<std::uint64_t,3> draws{}; };
    Pipeline* lookup(VkPipeline) noexcept;
    Command* lookup(VkCommandBuffer) noexcept;
    void refuse(Refusal) noexcept;
    bool resources(const VkPhysicalDeviceMemoryProperties&) noexcept;
    void writer() noexcept;
    void destroyResources() noexcept;
    bool updatePhase(VkQueue, unsigned) noexcept;
    VkDevice device_{};
    PFN_vkGetDeviceProcAddr gdpa_{};
#define DTP_FIELD(name) PFN_vk##name name##_{};
    DTP_FIELD(CreateBuffer) DTP_FIELD(DestroyBuffer) DTP_FIELD(GetBufferMemoryRequirements)
    DTP_FIELD(AllocateMemory) DTP_FIELD(FreeMemory) DTP_FIELD(BindBufferMemory)
    DTP_FIELD(MapMemory) DTP_FIELD(UnmapMemory)
    DTP_FIELD(CreateDescriptorSetLayout) DTP_FIELD(DestroyDescriptorSetLayout)
    DTP_FIELD(CreateDescriptorPool) DTP_FIELD(DestroyDescriptorPool) DTP_FIELD(AllocateDescriptorSets) DTP_FIELD(UpdateDescriptorSets)
    DTP_FIELD(CreatePipelineLayout) DTP_FIELD(DestroyPipelineLayout)
    DTP_FIELD(CreateShaderModule) DTP_FIELD(DestroyShaderModule) DTP_FIELD(CreateGraphicsPipelines) DTP_FIELD(DestroyPipeline)
    DTP_FIELD(CreateCommandPool) DTP_FIELD(DestroyCommandPool) DTP_FIELD(AllocateCommandBuffers)
    DTP_FIELD(BeginCommandBuffer) DTP_FIELD(EndCommandBuffer)
    DTP_FIELD(CmdUpdateBuffer) DTP_FIELD(CmdPipelineBarrier) DTP_FIELD(CmdBindPipeline) DTP_FIELD(CmdBindDescriptorSets)
    DTP_FIELD(CreateFence) DTP_FIELD(DestroyFence) DTP_FIELD(GetFenceStatus) DTP_FIELD(QueueSubmit)
#undef DTP_FIELD
    VkBuffer buffer_{};
    VkDeviceMemory memory_{};
    void* mapped_{};
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool descriptorPool_{};
    VkDescriptorSet descriptor_{};
    VkCommandPool commandPool_{};
    std::array<VkCommandBuffer, 4> phaseCommands_{};
    std::array<VkFence, 4> phaseFences_{};
    std::array<bool, 4> phaseSpent_{};
    std::array<Pipeline, MaximumPipelines> pipelines_{};
    std::array<Command, MaximumCommands> commands_{};
    std::array<ShaderIdentity,128> shaders_{};
    std::array<Route,64> routes_{};
    std::array<std::uint64_t,3> unknownRoutes_{};
    std::array<std::uint32_t, ShaderBytes / 4> shader_{};
    std::mutex mutex_;
    PhaseSequence sequence_;
    std::atomic<Refusal> refusal_{Refusal::Disabled};
    std::atomic<std::uint64_t> draws_{0}, missed_{0}, submits_{0}, revision_{0};
    std::atomic<std::uint64_t> shaderDropped_{0}, routesDropped_{0};
    std::atomic<unsigned> companionUsed_{0}, companionCreated_{0}, shaderCached_{0}, genericRoutes_{0};
    std::atomic<unsigned> queuedPhase_{3};
    std::atomic<bool> stop_{false}, collected_{false};
    std::atomic<bool> disabledFenceSubmitted_{false};
    std::thread writer_;
    VkQueue queue_{};
    std::uint32_t family_=UINT32_MAX;
    QueueProfile queueProfile_{};
    std::uint32_t nativeQueueCreateInfos_{}, nativeQueueCount_{}, featureSource_{};
    bool featureObserved_{}, featureEnabled_{};
    int directoryFd_ = -1;
    bool ready_ = false, released_ = false;
};
} // namespace dashboard_transition_probe
