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
constexpr const char* ShaderSha = "3439b80751b87307c16097490f036a50a161bda5f7ee446ac5550337144751de";
constexpr std::size_t ShaderBytes = 60876;
constexpr std::size_t PipelineTagWord = 1403;
constexpr std::uint32_t TagTemplate = 0x44545000;

enum class Refusal : unsigned {
    None, Disabled, Feature, QueueProfile, Dispatch, Artifact, Resource,
    PipelineCapacity, CommandCapacity, Recording, Submit, Queue, DeviceLost,
    Mutation, Collection, PhaseOrder
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
                 const VkPhysicalDeviceMemoryProperties&, int ownedDirectoryFd) noexcept;
    void pipeline(VkPipeline normal, VkPipelineCache, const VkGraphicsPipelineCreateInfo&,
                  const frame_pixel_ssbo_clone::Recipe&) noexcept;
    void pipelineRetiring(VkPipeline) noexcept;
    void queueRetrieved(std::uint32_t family, std::uint32_t index, VkQueue, bool ordinary = true) noexcept;
    void commandsAllocated(const VkCommandBufferAllocateInfo&, const VkCommandBuffer*, VkResult) noexcept;
    void commandsFreed(VkCommandPool, std::uint32_t, const VkCommandBuffer*) noexcept;
    void poolRetired(VkCommandPool) noexcept;
    void begin(VkCommandBuffer, const VkCommandBufferBeginInfo&, VkResult) noexcept;
    void end(VkCommandBuffer, VkResult) noexcept;
    void reset(VkCommandBuffer, VkResult) noexcept;
    void bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) noexcept;
    void unknown(VkCommandBuffer) noexcept;
    bool beforeDraw(VkCommandBuffer) noexcept;
    void afterDraw(VkCommandBuffer) noexcept;
    void beforeSubmit(VkQueue, bool ordinary) noexcept;
    void release(VkResult actualDeviceIdle) noexcept;
    Refusal refusal() const noexcept { return refusal_.load(std::memory_order_acquire); }
    bool prepared() const noexcept { return ready_; }
private:
    struct Pipeline { VkPipeline normal{}, instrumented{}; VkPipelineLayout layout{}; std::uint32_t tag{}; bool retired{}; };
    struct Command { VkCommandBuffer value{}; VkCommandPool pool{}; VkPipeline bound{}; bool primary{}, recording{}, eligible{}, swapped{}; };
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
    std::array<std::uint32_t, ShaderBytes / 4> shader_{};
    std::mutex mutex_;
    PhaseSequence sequence_;
    std::atomic<Refusal> refusal_{Refusal::Disabled};
    std::atomic<std::uint64_t> draws_{0}, missed_{0}, submits_{0}, revision_{0};
    std::atomic<unsigned> queuedPhase_{3};
    std::atomic<bool> stop_{false}, collected_{false};
    std::atomic<bool> disabledFenceSubmitted_{false};
    std::thread writer_;
    VkQueue queue_{};
    std::uint32_t family_{};
    int directoryFd_ = -1;
    bool ready_ = false, released_ = false;
};
} // namespace dashboard_transition_probe
