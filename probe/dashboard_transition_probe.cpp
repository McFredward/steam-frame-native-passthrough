#include "dashboard_transition_probe.h"
#include "../layer/pixel_snapshot_io.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dashboard_transition_probe {
QueueObservation observeQueueFamily(PFN_vkGetPhysicalDeviceQueueFamilyProperties query,
                                    VkPhysicalDevice physical,std::uint32_t family) noexcept {
    QueueObservation out;out.detailed=true;out.pfnPresent=query!=nullptr;out.rawFamily=family;
    if(!query)return out;
    std::uint32_t count=0;query(physical,&count,nullptr);out.firstCount=count;
    if(!count||count>64)return out;
    std::array<VkQueueFamilyProperties,64> families{};
    out.secondQueryCalled=true;query(physical,&count,families.data());out.secondCount=count;
    if(count==out.firstCount&&family<count)out.flags=families[family].queueFlags;
    return out;
}
namespace {
bool ownedRead(int directory, const char* name, void* bytes, std::size_t size) noexcept {
    const int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st{};
    bool valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
        (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size == static_cast<off_t>(size);
    std::size_t used = 0;
    while (valid && used < size) {
        const auto n = read(fd, static_cast<char*>(bytes) + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { valid = false; break; }
        used += static_cast<std::size_t>(n);
    }
    close(fd); return valid && used == size;
}
bool ownedWrite(int directory, const char* name, const void* bytes, std::size_t size) noexcept {
    const int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = frame_pixel_io::writeAll(fd, bytes, size); close(fd); return ok;
}
QueueProfile inspectQueue(const VkDeviceCreateInfo& info,VkQueueFlags flags,const QueueObservation* observation) noexcept {
    QueueProfile out;
    if(observation)out.query=*observation;
    else out.query.flags=flags; // Legacy owned-host fixture: flags only, no claimed query receipt.
    if(info.queueCreateInfoCount!=1)out.failures|=QueueCreateCount;
    if(!info.queueCreateInfoCount||!info.pQueueCreateInfos)out.failures|=QueueMissing;
    else {
        const auto& q=info.pQueueCreateInfos[0];out.available=true;out.type=std::uint32_t(q.sType);
        out.flags=q.flags;out.count=q.queueCount;out.rawFamily=q.queueFamilyIndex;
        if(q.sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO)out.failures|=QueueType;
        if(q.flags)out.failures|=QueueFlags;
        if(q.queueCount!=1)out.failures|=QueueCount;
        if(q.queueFamilyIndex==VK_QUEUE_FAMILY_IGNORED)out.failures|=QueueFamily;
        if(!q.pQueuePriorities)out.failures|=QueuePriorityMissing;
        else if(q.queueCount){
            out.priorityObserved=true;std::memcpy(&out.priorityBits,q.pQueuePriorities,4);
            if(!std::isfinite(q.pQueuePriorities[0])||q.pQueuePriorities[0]<0||q.pQueuePriorities[0]>1)out.failures|=QueuePriorityValue;
        }
        if(q.pNext){
            out.nextPresent=true;const auto* node=static_cast<const VkBaseInStructure*>(q.pNext);
            out.nextType=std::uint32_t(node->sType);out.nextHasNext=node->pNext!=nullptr;
            if(node->sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR)out.failures|=QueueNextType;
            if(node->pNext)out.failures|=QueueNextChain;
            // Do not follow any chain or read an unknown payload. This matches
            // the already reviewed Transaction::deviceCreated priority shape.
            if(node->sType==VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR&&!node->pNext){
                const auto priority=reinterpret_cast<const VkDeviceQueueGlobalPriorityCreateInfoKHR*>(node)->globalPriority;
                out.globalPriorityObserved=true;out.globalPriority=std::int32_t(priority);
                if(priority!=VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR&&priority!=VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR&&
                   priority!=VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR&&priority!=VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR)out.failures|=QueueGlobalPriority;
            }
        }
    }
    if(out.query.detailed){
        if(!out.query.pfnPresent)out.failures|=QueueQueryMissing;
        if(!out.query.firstCount||out.query.firstCount>64)out.failures|=QueueQueryCount;
        if(!out.query.secondQueryCalled||out.query.secondCount!=out.query.firstCount)out.failures|=QueueQueryChanged;
        if(out.query.rawFamily!=out.rawFamily||out.query.rawFamily>=out.query.secondCount)out.failures|=QueueQueryRange;
        if(out.query.flags!=flags)out.failures|=QueueQueryFlags;
    }
    if(!(flags&VK_QUEUE_GRAPHICS_BIT))out.failures|=QueueNotGraphics;
    return out;
}
} // namespace

Probe::~Probe() {
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    if (directoryFd_ >= 0) close(directoryFd_);
    // No Vulkan call in destructor; native device teardown supplies quiescence.
}
void Probe::refuse(Refusal value) noexcept {
    Refusal before = Refusal::None;
    if (refusal_.compare_exchange_strong(before, value)) ++revision_;
}
void Probe::prepare(VkDevice device, const VkDeviceCreateInfo& info, PFN_vkGetDeviceProcAddr gdpa,
                    const VkPhysicalDeviceMemoryProperties& properties, int directory, VkQueueFlags queueFlags,
                    const QueueObservation* observation) noexcept {
    if (directoryFd_ >= 0 || ready_ || released_) return;
    constexpr char magic[] = "SFNP_DASHBOARD_TRANSITION/1\n";
    char marker[sizeof(magic)-1]{};
    if (!ownedRead(directory, "dashboard-transition.enable", marker, sizeof(marker)) ||
        std::memcmp(marker, magic, sizeof(marker))) return;
    directoryFd_ = fcntl(directory, F_DUPFD_CLOEXEC, 3);
    if (directoryFd_ < 0) return;
    refusal_.store(Refusal::None); ++revision_;
    device_ = device; gdpa_ = gdpa;
    frame_pixel_ssbo_clone::FeatureFacts feature;
    frame_pixel_ssbo_clone::Refusal why;
    if (!frame_pixel_ssbo_clone::enabledFeature(info, true, feature, why)) refuse(Refusal::Feature);
    featureObserved_=feature.observed;featureEnabled_=feature.enabled;featureSource_=unsigned(feature.source);
    nativeQueueCreateInfos_=info.queueCreateInfoCount;
    if(info.queueCreateInfoCount==1&&info.pQueueCreateInfos)nativeQueueCount_=info.pQueueCreateInfos[0].queueCount;
    queueProfile_=inspectQueue(info,queueFlags,observation);
    family_=queueProfile_.rawFamily;
    if(queueProfile_.failures)refuse(Refusal::QueueProfile);
#define LOAD(name) name##_=reinterpret_cast<PFN_vk##name>(gdpa(device,"vk" #name)); if(!name##_)refuse(Refusal::Dispatch);
    LOAD(CreateBuffer) LOAD(DestroyBuffer) LOAD(GetBufferMemoryRequirements)
    LOAD(AllocateMemory) LOAD(FreeMemory) LOAD(BindBufferMemory) LOAD(MapMemory) LOAD(UnmapMemory)
    LOAD(CreateDescriptorSetLayout) LOAD(DestroyDescriptorSetLayout)
    LOAD(CreateDescriptorPool) LOAD(DestroyDescriptorPool) LOAD(AllocateDescriptorSets) LOAD(UpdateDescriptorSets)
    LOAD(CreatePipelineLayout) LOAD(DestroyPipelineLayout)
    LOAD(CreateShaderModule) LOAD(DestroyShaderModule) LOAD(CreateGraphicsPipelines) LOAD(DestroyPipeline)
    LOAD(CreateCommandPool) LOAD(DestroyCommandPool) LOAD(AllocateCommandBuffers) LOAD(BeginCommandBuffer) LOAD(EndCommandBuffer)
    LOAD(CmdUpdateBuffer) LOAD(CmdPipelineBarrier) LOAD(CmdBindPipeline) LOAD(CmdBindDescriptorSets)
    LOAD(CreateFence) LOAD(DestroyFence) LOAD(GetFenceStatus) LOAD(QueueSubmit)
#undef LOAD
    if (!ownedRead(directoryFd_, "dashboard-transition.spv", shader_.data(), ShaderBytes) ||
        frame_pixel_io::digest(shader_.data(), ShaderBytes) != ShaderSha ||
        shader_[PipelineTagWord] != TagTemplate) refuse(Refusal::Artifact);
    if (refusal() == Refusal::None) {
        ready_ = resources(properties);
        if (!ready_) refuse(Refusal::Resource);
    }
    try { writer_ = std::thread(&Probe::writer, this); }
    catch (...) { refuse(Refusal::Resource); ready_ = false; }
}
bool Probe::resources(const VkPhysicalDeviceMemoryProperties& properties) noexcept {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = BufferBytes; info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (CreateBuffer_(device_, &info, nullptr, &buffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{}; GetBufferMemoryRequirements_(device_, buffer_, &requirements);
    if (!requirements.size || requirements.size > 65536) return false;
    std::uint32_t memoryType = UINT32_MAX;
    for (unsigned i = 0; i < properties.memoryTypeCount && i < 32; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { memoryType = i; break; }
    if (memoryType == UINT32_MAX) return false;
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size; memory.memoryTypeIndex = memoryType;
    if (AllocateMemory_(device_, &memory, nullptr, &memory_) != VK_SUCCESS ||
        BindBufferMemory_(device_, buffer_, memory_, 0) != VK_SUCCESS ||
        MapMemory_(device_, memory_, 0, BufferBytes, 0, &mapped_) != VK_SUCCESS) return false;
    std::memset(mapped_, 0, BufferBytes); static_cast<std::uint32_t*>(mapped_)[0] = 3;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 1; layout.pBindings = &binding;
    if (CreateDescriptorSetLayout_(device_, &layout, nullptr, &setLayout_) != VK_SUCCESS) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    if (CreateDescriptorPool_(device_, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool_; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &setLayout_;
    if (AllocateDescriptorSets_(device_, &allocate, &descriptor_) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo region{buffer_, 0, BufferBytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &region;
    UpdateDescriptorSets_(device_, 1, &write, 0, nullptr);
    VkCommandPoolCreateInfo commandPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPool.queueFamilyIndex = family_;
    if (CreateCommandPool_(device_, &commandPool, nullptr, &commandPool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commands.commandPool = commandPool_; commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commands.commandBufferCount = 4;
    if (AllocateCommandBuffers_(device_, &commands, phaseCommands_.data()) != VK_SUCCESS) return false;
    for (unsigned phase = 0; phase < 4; ++phase) {
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (CreateFence_(device_, &fence, nullptr, &phaseFences_[phase]) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (BeginCommandBuffer_(phaseCommands_[phase], &begin) != VK_SUCCESS) return false;
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.buffer = buffer_; barrier.size = BufferBytes;
        CmdPipelineBarrier_(phaseCommands_[phase], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
        CmdUpdateBuffer_(phaseCommands_[phase], buffer_, 0, 4, &phase);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | (phase == 3 ? VK_ACCESS_SHADER_WRITE_BIT : 0);
        barrier.dstAccessMask = phase == 3 ? VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        CmdPipelineBarrier_(phaseCommands_[phase], VK_PIPELINE_STAGE_TRANSFER_BIT | (phase == 3 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                            phase == 3 ? VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 1, &barrier, 0, nullptr);
        if (EndCommandBuffer_(phaseCommands_[phase]) != VK_SUCCESS) return false;
    }
    return true;
}
Probe::Pipeline* Probe::lookup(VkPipeline value) noexcept {
    for (auto& pipeline : pipelines_) if (pipeline.normal == value && !pipeline.retired) return &pipeline;
    return nullptr;
}
Probe::Command* Probe::lookup(VkCommandBuffer value) noexcept {
    for (auto& command : commands_) if (command.value == value) return &command;
    return nullptr;
}
void Probe::pipeline(VkPipeline normal, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo& original,
                     const frame_pixel_ssbo_clone::Recipe& recipe) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || refusal() != Refusal::None) return;
    if (!recipe.feature.complete || !recipe.feature.enabled || !recipe.definitionComplete ||
        original.stageCount != 2 || !original.pStages || !recipe.provenance[0].value) { refuse(Refusal::Feature); return; }
    Pipeline* target = nullptr;
    for (auto& p : pipelines_) if (!p.normal) { target = &p; break; }
    if (!target) { refuse(Refusal::PipelineCapacity); return; }
    target->normal = normal; target->tag = TagTemplate + static_cast<unsigned>(target - pipelines_.data()) + 1;
    VkDescriptorSetLayout nativeSet{};
    std::memcpy(&nativeSet, &recipe.provenance[0].value, sizeof(nativeSet));
    const std::array<VkDescriptorSetLayout, 2> sets{{nativeSet, setLayout_}};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 2; layout.pSetLayouts = sets.data();
    layout.pushConstantRangeCount = recipe.layout.count; layout.pPushConstantRanges = recipe.layout.ranges.data();
    if (CreatePipelineLayout_(device_, &layout, nullptr, &target->layout) != VK_SUCCESS) { refuse(Refusal::Resource); return; }
    shader_[PipelineTagWord] = target->tag;
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = ShaderBytes; module.pCode = shader_.data(); VkShaderModule fragment{};
    if (CreateShaderModule_(device_, &module, nullptr, &fragment) != VK_SUCCESS) { refuse(Refusal::Resource); return; }
    auto stages = std::array<VkPipelineShaderStageCreateInfo, 2>{{original.pStages[0], original.pStages[1]}};
    for (auto& stage : stages) if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) stage.module = fragment;
    auto info = original; info.layout = target->layout; info.pStages = stages.data();
    const auto result = CreateGraphicsPipelines_(device_, cache, 1, &info, nullptr, &target->instrumented);
    DestroyShaderModule_(device_, fragment, nullptr);
    shader_[PipelineTagWord] = TagTemplate;
    if (result != VK_SUCCESS || !target->instrumented) refuse(Refusal::Resource);
    ++revision_;
}
void Probe::pipelineRetiring(VkPipeline value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* p = lookup(value)) p->retired = true;
    for(auto& route:routes_)if(route.handle==frame_pixel_ssbo_clone::handle(value))route.retired=1;
    // Keep companion resources until actual device quiescence. Native handle
    // destruction invalidates its recorded uses; never recycle a pipeline tag.
}
void Probe::shaderObserved(VkShaderModule value,const VkShaderModuleCreateInfo& info) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || !info.pCode || !info.codeSize || info.codeSize > 256u*1024u) return;
    // Scalar module hashes only; no shader body is recorded or redistributed.
    ShaderIdentity* target=nullptr;
    for(auto& s:shaders_)if(s.handle==frame_pixel_ssbo_clone::handle(value)){target=&s;break;}
    if(!target)for(auto& s:shaders_)if(!s.handle){target=&s;break;}
    if(!target){refuse(Refusal::PipelineCapacity);return;}
    target->handle=frame_pixel_ssbo_clone::handle(value);target->bytes=static_cast<std::uint32_t>(info.codeSize);
    frame_receiver::Sha256 hash;hash.update(info.pCode,info.codeSize);target->sha=hash.finish();
}
void Probe::pipelineObserved(VkPipeline value,const VkGraphicsPipelineCreateInfo& info,unsigned reason) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!ready_)return;
    Route* target=nullptr;
    for(auto& r:routes_)if(r.handle==frame_pixel_ssbo_clone::handle(value)&&!r.retired){target=&r;break;}
    if(!target)for(auto& r:routes_)if(!r.handle){target=&r;break;}
    if(!target){refuse(Refusal::PipelineCapacity);return;}
    target->handle=frame_pixel_ssbo_clone::handle(value);target->nativeReason=reason;
    if(auto* p=lookup(value))target->tag=p->tag;
    if(info.stageCount<=8&&info.pStages)for(unsigned i=0;i<info.stageCount;++i)
        for(const auto& s:shaders_)if(s.handle==frame_pixel_ssbo_clone::handle(info.pStages[i].module)){
            if(info.pStages[i].stage==VK_SHADER_STAGE_VERTEX_BIT)target->vs=s.sha;
            if(info.pStages[i].stage==VK_SHADER_STAGE_FRAGMENT_BIT)target->ps=s.sha;
        }
    ++revision_;
}
void Probe::queueRetrieved(std::uint32_t family, std::uint32_t index, VkQueue value, bool ordinary) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return;
    if (!ordinary || family != family_ || index != 0 || !value || (queue_ && queue_ != value)) refuse(Refusal::Queue);
    else queue_ = value;
}
void Probe::commandsAllocated(const VkCommandBufferAllocateInfo& info, const VkCommandBuffer* values, VkResult result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || result != VK_SUCCESS || !values) return;
    if (info.commandBufferCount > MaximumCommands) { refuse(Refusal::CommandCapacity); return; }
    for (unsigned i = 0; i < info.commandBufferCount; ++i) {
        Command* target = nullptr;
        for (auto& c : commands_) if (!c.value) { target = &c; break; }
        if (!target) { refuse(Refusal::CommandCapacity); return; }
        *target = {}; target->value = values[i]; target->pool = info.commandPool;
        target->primary = !info.pNext && info.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    }
}
void Probe::commandsFreed(VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* values) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count > MaximumCommands || (count && !values)) { refuse(Refusal::CommandCapacity); return; }
    for (unsigned i = 0; i < count; ++i) if (auto* c = lookup(values[i])) {
        if (c->pool != pool) refuse(Refusal::Recording);
        *c = {};
    }
}
void Probe::poolRetired(VkCommandPool pool,bool destroyed) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : commands_) if (c.pool == pool) {
        if(destroyed)c={};
        else { c.bound = {}; c.usedRoutes=0;c.unknownRoute=false;c.recording = c.eligible = false; }
    }
}
void Probe::begin(VkCommandBuffer value, const VkCommandBufferBeginInfo& info, VkResult result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* c = lookup(value)) {
        c->bound = {}; c->usedRoutes=0;c->unknownRoute=false;c->swapped = false; c->recording = result == VK_SUCCESS;
        c->eligible = c->recording && c->primary && !info.pNext && !info.pInheritanceInfo &&
            !(info.flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
    }
}
void Probe::end(VkCommandBuffer value, VkResult result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* c = lookup(value)) { c->recording = false; if (result != VK_SUCCESS) c->eligible = false; }
}
void Probe::reset(VkCommandBuffer value, VkResult result) noexcept {
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* c = lookup(value)) { c->bound = {}; c->usedRoutes=0;c->unknownRoute=false;c->recording = c->eligible = c->swapped = false; }
}
void Probe::bind(VkCommandBuffer value, VkPipelineBindPoint point, VkPipeline pipeline) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* c = lookup(value)) if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) c->bound = pipeline;
}
void Probe::unknown(VkCommandBuffer value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* c = lookup(value)) {c->eligible = false;c->unknownRoute=true;}
}
void Probe::nativeDescriptorBinding(VkCommandBuffer value,VkPipelineBindPoint point,std::uint32_t first,std::uint32_t count) noexcept {
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&count&&(first>0||count>1))unknown(value);
}
bool Probe::beforeDraw(VkCommandBuffer value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* c = lookup(value); auto* p = c ? lookup(c->bound) : nullptr;
    if(c){
        bool found=false;
        for(std::size_t i=0;i<routes_.size();++i)if(routes_[i].handle==frame_pixel_ssbo_clone::handle(c->bound)&&!routes_[i].retired){
            c->usedRoutes|=std::uint64_t(1)<<i;found=true;break;
        }
        if(!found)c->unknownRoute=true;
    }
    if (!ready_ || refusal() != Refusal::None || !c || !p || !p->instrumented || !c->recording || !c->eligible) {
        ++missed_; return false;
    }
    CmdBindPipeline_(value, VK_PIPELINE_BIND_POINT_GRAPHICS, p->instrumented);
    CmdBindDescriptorSets_(value, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &descriptor_, 0, nullptr);
    c->swapped = true; ++draws_; return true;
}
void Probe::afterDraw(VkCommandBuffer value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* c = lookup(value)) if (c->swapped) {
        CmdBindPipeline_(value, VK_PIPELINE_BIND_POINT_GRAPHICS, c->bound); c->swapped = false;
    }
}
bool Probe::updatePhase(VkQueue queue, unsigned phase) noexcept {
    if (phase > 3 || phaseSpent_[phase]) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &phaseCommands_[phase];
    const auto result = QueueSubmit_(queue, 1, &submit, phaseFences_[phase]);
    phaseSpent_[phase] = true;
    if (result != VK_SUCCESS) { refuse(result == VK_ERROR_DEVICE_LOST ? Refusal::DeviceLost : Refusal::Submit); return false; }
    queuedPhase_.store(phase, std::memory_order_release);
    if (phase == 3) disabledFenceSubmitted_.store(true, std::memory_order_release);
    ++revision_; return true;
}
void Probe::beforeSubmit(VkQueue queue, bool ordinary, std::uint32_t count, const VkSubmitInfo* submits) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return;
    ++submits_;
    if (queue != queue_ || !queue_) { refuse(Refusal::Queue); return; }
    if (!ordinary) refuse(Refusal::Submit);
    const auto requested = refusal() == Refusal::None ? sequence_.requested.load(std::memory_order_acquire) : 3;
    if (requested != queuedPhase_.load(std::memory_order_acquire))
        updatePhase(queue, requested);
    const auto phase=queuedPhase_.load(std::memory_order_acquire);
    if(ordinary&&phase<3&&count<=128&&(!count||submits))for(unsigned i=0;i<count;++i)
        if(submits[i].commandBufferCount<=MaximumCommands&&(!submits[i].commandBufferCount||submits[i].pCommandBuffers))
            for(unsigned n=0;n<submits[i].commandBufferCount;++n){
                const auto* c=lookup(submits[i].pCommandBuffers[n]);
                if(!c){++unknownRoutes_[phase];continue;}
                for(std::size_t r=0;r<routes_.size();++r)if(c->usedRoutes&(std::uint64_t(1)<<r))++routes_[r].draws[phase];
                if(c->unknownRoute)++unknownRoutes_[phase];
            }
}
void Probe::writer() noexcept {
    // Immutable copied cold values only. The compositor callbacks never write
    // files, and a future rejection exposes the exact failed predicate set.
    const auto& q=queueProfile_;
    char queueOutput[1024]{};
    const auto queueLength=std::snprintf(queueOutput,sizeof(queueOutput),
        "{\"schema\":\"sfnp-dashboard-queue-profile-v1\",\"observed_device\":%llu,\"failure_bits\":%u,\"available\":%s,\"type\":%u,\"flags\":%u,\"count\":%u,\"raw_family\":%u,\"priority_observed\":%s,\"priority_bits\":%u,\"next_present\":%s,\"next_type\":%u,\"next_has_next\":%s,\"global_priority_observed\":%s,\"global_priority\":%d,\"query_detailed\":%s,\"query_pfn\":%s,\"query_first_count\":%u,\"query_second_called\":%s,\"query_second_count\":%u,\"query_raw_family\":%u,\"query_flags\":%u}\n",
        static_cast<unsigned long long>(frame_pixel_ssbo_clone::handle(device_)),q.failures,q.available?"true":"false",q.type,q.flags,q.count,q.rawFamily,
        q.priorityObserved?"true":"false",q.priorityBits,q.nextPresent?"true":"false",q.nextType,q.nextHasNext?"true":"false",
        q.globalPriorityObserved?"true":"false",q.globalPriority,q.query.detailed?"true":"false",q.query.pfnPresent?"true":"false",
        q.query.firstCount,q.query.secondQueryCalled?"true":"false",q.query.secondCount,q.query.rawFamily,q.query.flags);
    if(queueLength<=0||std::size_t(queueLength)>=sizeof(queueOutput)||
       !ownedWrite(directoryFd_,"dashboard-transition-queue-profile.json",queueOutput,std::size_t(queueLength)))refuse(Refusal::Collection);
    unsigned update = 0; std::uint64_t previous = UINT64_MAX;bool terminalWritten=false;
    while (!stop_.load(std::memory_order_acquire)) {
        char command[2]{};
        if (ownedRead(directoryFd_, "dashboard-transition.phase", command, 2) && command[1] == '\n' && command[0] >= '0' && command[0] <= '3') {
            if (!sequence_.request(static_cast<unsigned>(command[0] - '0'))) refuse(Refusal::PhaseOrder);
        }
        if (!collected_.load(std::memory_order_acquire) && disabledFenceSubmitted_.load(std::memory_order_acquire)) {
            const auto completed = GetFenceStatus_(device_, phaseFences_[3]);
            if (completed == VK_SUCCESS) {
                if (!ownedWrite(directoryFd_, "dashboard-transition-records.bin", mapped_, BufferBytes)) refuse(Refusal::Collection);
                std::array<Route,64> routes;
                std::array<std::uint64_t,3> unknown;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    routes=routes_;unknown=unknownRoutes_;
                }
                if(!ownedWrite(directoryFd_,"dashboard-transition-routes.bin",routes.data(),sizeof(routes)) ||
                   !ownedWrite(directoryFd_,"dashboard-transition-unknown-routes.bin",unknown.data(),sizeof(unknown)))refuse(Refusal::Collection);
                collected_.store(true, std::memory_order_release); ++revision_;
            } else if (completed != VK_NOT_READY) refuse(Refusal::DeviceLost);
        }
        // Pipeline creation/route revisions never consume the phase-control
        // budget. This key has at most12 monotonic lifecycle transitions.
        const auto revision = (std::uint64_t(unsigned(refusal()))<<16) |
            (std::uint64_t(sequence_.requested.load())<<8) | (std::uint64_t(queuedPhase_.load())<<4) |
            std::uint64_t(collected_.load());
        if ((revision != previous && update < 24) || (collected_.load()&&!terminalWritten)) {
            char output[1024]{}, name[64]{};
            const auto length = std::snprintf(output, sizeof(output),
                "{\"schema\":\"sfnp-dashboard-transition-status-v1\",\"observed_device\":%llu,\"ready\":%s,\"refusal\":%u,\"requested_phase\":%u,\"queued_phase\":%u,\"instrumented_draws\":%llu,\"unobserved_draws\":%llu,\"submits\":%llu,\"collected\":%s,\"fragment_stores_observed\":%s,\"fragment_stores_enabled\":%s,\"feature_source\":%u,\"native_queue_create_infos\":%u,\"native_queue_count\":%u,\"native_queue_family\":%u,\"queue_failure_bits\":%u,\"visibility_claim\":false,\"overlay_transport_claim\":false}\n",
                static_cast<unsigned long long>(frame_pixel_ssbo_clone::handle(device_)),ready_ ? "true" : "false", unsigned(refusal()), sequence_.requested.load(), queuedPhase_.load(),
                static_cast<unsigned long long>(draws_.load()), static_cast<unsigned long long>(missed_.load()),
                static_cast<unsigned long long>(submits_.load()), collected_.load() ? "true" : "false",
                featureObserved_?"true":"false",featureEnabled_?"true":"false",featureSource_,nativeQueueCreateInfos_,nativeQueueCount_,family_,q.failures);
            if(update<24){
                std::snprintf(name, sizeof(name), "dashboard-transition-status-%02u.json", update++);
                if (length > 0 && std::size_t(length) < sizeof(output)) ownedWrite(directoryFd_, name, output, std::size_t(length));
            }
            if(collected_.load()&&!terminalWritten&&length>0&&std::size_t(length)<sizeof(output)){
                ownedWrite(directoryFd_,"dashboard-transition-terminal.json",output,std::size_t(length));terminalWritten=true;
            }
            previous = revision;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
void Probe::destroyResources() noexcept {
    for (auto& p : pipelines_) {
        if (p.instrumented) DestroyPipeline_(device_, p.instrumented, nullptr);
        if (p.layout) DestroyPipelineLayout_(device_, p.layout, nullptr);
    }
    if (commandPool_) DestroyCommandPool_(device_, commandPool_, nullptr);
    for (auto fence : phaseFences_) if (fence) DestroyFence_(device_, fence, nullptr);
    if (descriptorPool_) DestroyDescriptorPool_(device_, descriptorPool_, nullptr);
    if (setLayout_) DestroyDescriptorSetLayout_(device_, setLayout_, nullptr);
    if (mapped_) UnmapMemory_(device_, memory_);
    if (buffer_) DestroyBuffer_(device_, buffer_, nullptr);
    if (memory_) FreeMemory_(device_, memory_, nullptr);
}
void Probe::release(VkResult actualDeviceIdle) noexcept {
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    if (released_) return;
    released_ = true;
    if (actualDeviceIdle == VK_SUCCESS || actualDeviceIdle == VK_ERROR_DEVICE_LOST) destroyResources();
    // Unproven native shutdown retains children for vkDestroyDevice; never free
    // objects referenced by an executable/pending command buffer speculatively.
    ready_ = false;
}
} // namespace dashboard_transition_probe
