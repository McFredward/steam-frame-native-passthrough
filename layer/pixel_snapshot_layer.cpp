// Default mode is an explicit-pattern carriage diagnostic with an owned P010
// copy and an optional manually armed, color-preserving SSBO observer.
// FRAME_PIXEL_SYNTHETIC_PRESENTER instead opts into cold creation-scoped guarded
// fragment replacement, without draw hooks, readbacks or camera configuration.
// Neither mode modifies the PC sender or reads private native process memory.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "pixel_capture_tracker.h"
#include "pixel_snapshot_selection.h"
#include "pixel_readback.h"
#include "pixel_snapshot_io.h"
#include "pixel_refusal_status.h"
#include "pixel_sampler_probe.h"
#include "pixel_sampler_admission.h"
#include "pixel_sampler_format.h"
#include "pixel_sampler_routes.h"
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
#include "../probe/dashboard_transition_probe.h"
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
#if !defined(FRAME_PIXEL_SSBO_DEFINITION_JOURNAL) || defined(FRAME_PIXEL_SSBO_TRANSACTION) || defined(FRAME_PIXEL_UNIFORM_COMPANION)
#error "Synthetic presenter requires only the definition journal; SSBO/uniform modes are mutually exclusive"
#endif
#include "native_presenter_receiver.h"
#include "pixel_ssbo_profile_transform.h"
#endif
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
#include "pixel_ssbo_definition_journal.h"
#include "pixel_definition_report.h"
#include "pixel_observation_boundary.h"
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
#include "pixel_ssbo_receiver_bridge.h"
#include "pixel_renderpass_recording_contract.h"
#include "pixel_native_driver_lease.h"
#endif
#endif
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
#include "pixel_uniform_copy.h"
#include "pixel_uniform_metadata.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#define PIXEL_EXPORT extern "C" __attribute__((visibility("default")))
namespace {
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
__attribute__((used)) const char DashboardDiagnosticBuildMarker[]="SFNP_DASHBOARD_TRANSITION_PROBE/1";
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
__attribute__((used)) const char SyntheticPresenterBuildMarker[]="GHVR_SYNTHETIC_PRESENTER_COLD/1";
__attribute__((used)) const char ScenePresenterBuildMarker[]="GHVR_SCENE_PRESENTER_COLD/1";
#ifdef FRAME_PIXEL_RELEASE_RECEIVER
__attribute__((used)) const char ReleaseReceiverBuildMarker[]="SFNP_RELEASE_RECEIVER/1";
#endif
namespace presenter=frame_native_presenter_receiver;
#endif
using frame_pixel_capture::LiveTracker;
using frame_pixel_capture::Candidate;
using frame_pixel_capture::SubmitTicket;
using frame_pixel_capture::RetirementTicket;
struct Session {
    frame_pixel_io::Config config;
    std::unique_ptr<LiveTracker> tracker;
    std::atomic<bool> armed{false}, deviceClaimed{false};
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    presenter::Source presenterSource;
    presenter::Counters presenterCounters;
    std::mutex presenterWriterMutex;
    std::thread presenterWriter;
    std::atomic<bool> presenterStop{false}, presenterWriterFailed{false};
    std::atomic<unsigned> presenterDevices{0};
    // Written by the single joined writer across device lifetimes, never reset.
    unsigned presenterStatusUpdates=0;
#endif
    bool allowed = false;
    explicit Session(bool inert=false) noexcept {
        if(inert)return;
        try {
        char path[4096]{};
        const auto count = readlink("/proc/self/exe", path, sizeof(path)-1);
        if (count <= 0) return;
        path[count] = 0;
        const auto* base = std::strrchr(path, '/');
        if (std::strcmp(base ? base+1 : path, "vrcompositor")) return;
        // Host fixtures may select their own private config. Deployment does
        // not add process/service environment or preload/audit libraries.
#ifdef FRAME_PIXEL_RELEASE_RECEIVER
        if(!frame_pixel_io::releaseProcessMatches())return;
        const auto releasePath=frame_pixel_io::defaultConfigPath();
        if (!frame_pixel_io::load(releasePath.c_str(),config)) return;
#else
        const auto* overridePath = std::getenv("GHVR_PIXEL_SNAPSHOT_CONFIG");
        if (!frame_pixel_io::load(overridePath ? overridePath : frame_pixel_io::ConfigPath, config)) return;
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
        // Failure is reportable by the cold status writer, but can never admit
        // a replacement. No per-frame config, marker or artifact reads exist.
        presenterSource.prepareCold(config.directoryFd);
#ifdef FRAME_PIXEL_RELEASE_RECEIVER
        if(!presenterSource.ready() || presenterSource.profile()!=frame_native_presenter::Profile::SceneContribution)return;
#endif
#endif
        // One native process may claim a prepared run. A crash/reboot/restart
        // never re-arms an old capture, even if its temporary manifest remains.
        const int claim=frame_pixel_io::create(config,"launch.claim");
        if(claim<0)return;
        const bool claimed=frame_pixel_io::writeAll(claim,"GHVR_PIXEL_CLAIM/1\n",19);
        close(claim);if(!claimed)return;
#ifndef FRAME_PIXEL_SYNTHETIC_PRESENTER
        tracker = std::make_unique<LiveTracker>();
#endif
        allowed = true;
        } catch(...) { allowed=false; tracker.reset(); }
    }
};
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
std::atomic<Session*>& presenterSessionSlot() {
    static std::atomic<Session*> value{nullptr};return value;
}
Session& session() {
    // Calling a GPA/create callback before negotiation cannot initiate file I/O.
    // The loader's explicit cold negotiation is the sole preparation boundary.
    static Session inert(true);
    auto* value=presenterSessionSlot().load(std::memory_order_acquire);
    return value?*value:inert;
}
void preparePresenterSessionCold() noexcept {
    static std::once_flag prepared;
    try{std::call_once(prepared,[]{
        auto* value=new Session();
        presenterSessionSlot().store(value,std::memory_order_release);
    });}catch(...){/* Allocation failure keeps the optional path inert. */}
}
#else
Session& session() {
    // A native process may exit without explicit Vulkan teardown. Keep cold
    // diagnostic state valid until process exit rather than race a live writer
    // against the C++ global-destructor order. Explicit device teardown still
    // joins writers and frees every owned GPU child.
    static Session inert(true);
    static auto* value = []() noexcept {try{return new Session();}catch(...){return static_cast<Session*>(nullptr);}}();
    return value ? *value : inert;
}
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
void presenterWriter(Session* s) noexcept {
    std::uint64_t previous=0;
    auto& updates=s->presenterStatusUpdates;
    for(;updates<presenter::MaximumStatusUpdates;){
        const auto state=s->presenterCounters.snapshot();
        if(state.revision!=previous){
            ++updates;
            if(!presenter::writeStatusCold(s->config.directoryFd,s->config.captureId.c_str(),s->presenterSource,
                    state,updates,updates==presenter::MaximumStatusUpdates)){
                s->presenterWriterFailed.store(true,std::memory_order_release);return;
            }
            previous=state.revision;
        }
        if(s->presenterStop.load(std::memory_order_acquire))return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
void presenterDeviceStarted(VkDevice device,std::uint32_t api) noexcept {
    auto& s=session();s.presenterDevices.fetch_add(1,std::memory_order_acq_rel);
    s.presenterCounters.deviceCreated(device,api);
    try{
        std::lock_guard<std::mutex> lock(s.presenterWriterMutex);
        if(!s.presenterWriter.joinable()){
            s.presenterStop.store(false,std::memory_order_release);
            s.presenterWriter=std::thread(presenterWriter,&s);
        }
    }catch(...){s.presenterWriterFailed.store(true,std::memory_order_release);}
}
void presenterDeviceStopped() noexcept {
    auto& s=session();s.presenterCounters.deviceDestroyed();
    if(s.presenterDevices.fetch_sub(1,std::memory_order_acq_rel)!=1)return;
    try{
        std::lock_guard<std::mutex> lock(s.presenterWriterMutex);
        if(s.presenterDevices.load(std::memory_order_acquire))return;
        s.presenterStop.store(true,std::memory_order_release);
        if(s.presenterWriter.joinable())s.presenterWriter.join();
    }catch(...){s.presenterWriterFailed.store(true,std::memory_order_release);}
}
#endif
template<class T> void* dispatchKey(T value) { return *reinterpret_cast<void**>(value); }
struct Instance {
    VkInstance native{};
    std::uint32_t apiVersion = VK_API_VERSION_1_0;
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_GetPhysicalDeviceProcAddr gpdpa{};
};
enum class SamplerRefusal : unsigned { None, Device, ComputeState, Queue, Format, Extent, Conversion, Profile };
const char* refusalName(SamplerRefusal refusal) noexcept {
    switch(refusal) {
    case SamplerRefusal::Device:return "device_not_admitted";
    case SamplerRefusal::ComputeState:return "prior_compute_or_query_state";
    case SamplerRefusal::Queue:return "queue_without_compute";
    case SamplerRefusal::Format:return "unqueried_native_image_profile";
    case SamplerRefusal::Extent:return "extent_outside_reader_budget";
    case SamplerRefusal::Conversion:return "incomplete_native_conversion";
    case SamplerRefusal::Profile:return "native_view_or_conversion_profile";
    default:return "none";
    }
}
struct Device {
    VkDevice native{};
    std::uint64_t id = 0;
    PFN_vkGetDeviceProcAddr gdpa{};
#define FIELD(name) PFN_vk##name name{};
    FIELD(DestroyDevice) FIELD(DeviceWaitIdle) FIELD(CreateShaderModule) FIELD(DestroyShaderModule)
    FIELD(CreateGraphicsPipelines) FIELD(DestroyPipeline) FIELD(CreateCommandPool)
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    FIELD(CreateRenderPass) FIELD(CreateRenderPass2) FIELD(CreateRenderPass2KHR) FIELD(DestroyRenderPass)
#if defined(FRAME_PIXEL_SSBO_TRANSACTION) || defined(FRAME_DASHBOARD_TRANSITION_PROBE)
    FIELD(GetDeviceQueue) FIELD(GetDeviceQueue2) FIELD(CmdDrawIndirectCount) FIELD(CmdDrawIndexedIndirectCount) FIELD(CmdDrawIndirectCountKHR) FIELD(CmdDrawIndexedIndirectCountKHR)
#endif
#endif
    FIELD(AllocateCommandBuffers) FIELD(FreeCommandBuffers) FIELD(BeginCommandBuffer) FIELD(EndCommandBuffer)
    FIELD(ResetCommandBuffer) FIELD(ResetCommandPool) FIELD(DestroyCommandPool) FIELD(CmdBindPipeline)
    FIELD(CmdDraw) FIELD(CmdDrawIndexed) FIELD(CmdDrawIndirect) FIELD(CmdDrawIndexedIndirect)
    FIELD(CmdBindShadersEXT)
    FIELD(CmdDispatch) FIELD(CmdDispatchIndirect) FIELD(CmdDispatchBase) FIELD(CmdDispatchBaseKHR)
    FIELD(CmdBeginQuery) FIELD(CmdBeginQueryIndexedEXT)
    FIELD(CmdExecuteCommands) FIELD(CmdBeginRenderPass) FIELD(CmdNextSubpass) FIELD(CmdEndRenderPass)
    FIELD(CmdBeginRenderPass2) FIELD(CmdNextSubpass2) FIELD(CmdEndRenderPass2)
    FIELD(CmdBeginRenderPass2KHR) FIELD(CmdNextSubpass2KHR) FIELD(CmdEndRenderPass2KHR)
    FIELD(CmdBeginRendering) FIELD(CmdEndRendering) FIELD(CmdBeginRenderingKHR) FIELD(CmdEndRenderingKHR) FIELD(QueueSubmit) FIELD(QueueSubmit2) FIELD(QueueSubmit2KHR)
#define BINDING_FIELD(name) FIELD(name)
#include "passive_binding_commands.inc"
#undef BINDING_FIELD
#undef FIELD
    frame_pixels::Readback readback;
    frame_pixel_sampler::SamplerProbe sampler;
    frame_pixel_sampler::Dispatch samplerDispatch;
    frame_pixel_sampler::DeviceEligibility samplerEligibility;
    frame_pixel_sampler_format::Support samplerFormats;
    std::array<VkQueueFlags,64> queueFlags{};
    std::uint32_t queueFamilyCount = 0;
    bool nativeComputeQueue = false;
    std::uint32_t instanceApiVersion = 0;
    char unreviewedExtension[VK_MAX_EXTENSION_NAME_SIZE]{};
    VkResult samplerPrepareResult = VK_NOT_READY; // No driver call until samplerPrepareAttempted.
    bool samplerPrepareAttempted = false;
    bool samplerRecorded = false;
    std::atomic<unsigned> queryRoutesUsed{0};
    std::atomic<SamplerRefusal> samplerRefusal{SamplerRefusal::None};
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
#if defined(FRAME_PIXEL_SSBO_TRANSACTION) || defined(FRAME_PIXEL_SYNTHETIC_PRESENTER)
    frame_pixel_ssbo_journal::Journal definitions{1,{},frame_pixel_ssbo_journal::PendingCount1::RetainFacts};
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    frame_pixel_ssbo_receiver::Bridge bridge;
    // Immutable NEXT inputs from the actual successful DeviceCreate/load path.
    // Only the cold writer resolves provider mappings and verifies their bytes.
    frame_pixel_native_driver::Inputs nativeSubpassInputs{};
    frame_pixel_native_driver::Lease nativeSubpassLease;
#endif
#else
    frame_pixel_ssbo_journal::Journal definitions;
#endif
    frame_pixel_observation::Boundary definitionBoundary;
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    std::atomic<bool> presenterInterfaceAuthorized{false};
    bool presenterRegistered=false;
#endif
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    frame_uniform_copy::CopyRecorder uniformCopy;
    frame_uniform_copy::Dispatch uniformDispatch;
    frame_pixel_uniform::Limits uniformLimits{};
    bool uniformPrepared = false, uniformRecorded = false;
    std::atomic<unsigned> uniformRuntimeRefusal{0}, uniformRefusal{0}, uniformContractRefusal{0}, uniformResourceRefusal{0}, uniformOffendingBinding{UINT32_MAX};
#endif
    Candidate candidate{};
    std::thread writer;
    std::atomic<bool> stop{false}, selected{false};
    bool ownsCapture = false;
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
    dashboard_transition_probe::Probe dashboardProbe;
#endif
};
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
template<class F> void observeDefinitions(Device* d,F&& observe) noexcept {
    d->definitionBoundary.observe([&]{observe(d->definitions);});
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(d->definitionBoundary.failed())d->bridge.definitionObservationFailed();
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    if(d->definitionBoundary.failed())session().presenterCounters.historyFailed();
#endif
}
#endif
bool uniformCompanionEnabled() noexcept {
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    return true;
#else
    return false;
#endif
}
bool uniformCompanionReady(const Device& device) noexcept {
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    return device.uniformPrepared;
#else
    (void)device;
    return false;
#endif
}
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
enum class UniformRuntimeRefusal : unsigned { None, ColdPreparation, LastDrawMetadata, StagingCapacity };
void uniformRefuse(Device& device, UniformRuntimeRefusal reason, unsigned tracker = 0,
                   unsigned resources = 0, unsigned binding = UINT32_MAX, unsigned contract = 0) noexcept {
    unsigned empty = 0;
    if (!device.uniformRuntimeRefusal.compare_exchange_strong(empty, UINT32_MAX, std::memory_order_acq_rel)) return;
    device.uniformRefusal.store(tracker,std::memory_order_relaxed);
    device.uniformContractRefusal.store(contract,std::memory_order_relaxed);
    device.uniformResourceRefusal.store(resources,std::memory_order_relaxed);
    device.uniformOffendingBinding.store(binding,std::memory_order_relaxed);
    device.uniformRuntimeRefusal.store(static_cast<unsigned>(reason),std::memory_order_release);
}
#endif
struct Contexts {
    std::mutex mutex;
    std::unordered_map<void*, std::unique_ptr<Instance>> instances;
    std::unordered_map<void*, std::unique_ptr<Device>> devices;
};
Contexts& contexts() { static auto* value=new Contexts();return *value; }
std::atomic<std::uint64_t> nextDevice{0};
template<class T> Instance* instanceFor(T value) {
    if(!value)return nullptr;
    auto& c=contexts(); std::lock_guard<std::mutex> lock(c.mutex);
    const auto found = c.instances.find(dispatchKey(value));
    return found == c.instances.end() ? nullptr : found->second.get();
}
template<class T> Device* deviceFor(T value) {
    if(!value)return nullptr;
    auto& c=contexts(); std::lock_guard<std::mutex> lock(c.mutex);
    const auto found = c.devices.find(dispatchKey(value));
    return found == c.devices.end() ? nullptr : found->second.get();
}
template<class F> void observeBindingsAt(const char* site, F observer) {
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    (void)site;(void)observer;
#else
    if (session().allowed) session().tracker->observe(observer, site);
#endif
}
void status(int fd, const char* name, int result = 0) {
    if (fd < 0) return;
    char bytes[256]{};
    const auto count=std::snprintf(bytes,sizeof(bytes),
        "{\"event\":\"%s\",\"vk_result\":%d,\"mask_enabled\":false,\"native_frame_association_proven\":false}\n",name,result);
    if(count>0 && static_cast<std::size_t>(count)<sizeof(bytes))frame_pixel_io::writeAll(fd,bytes,static_cast<std::size_t>(count));
}
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
// Cold serialization only. Callback code owns finite numeric facts; formatting
// and I/O occur exclusively on this background writer or in offline fixtures.
constexpr std::size_t RecordingContextLineBytes=2048;
void recordingContextJson(std::ostream& out,const frame_pixel_ssbo_transaction::RecordingContext& context) {
    namespace tx=frame_pixel_ssbo_transaction;const auto& in=context.input;
    out<<"{\"operation\":\""<<tx::operationName(in.operation)<<"\",\"source\":"<<unsigned(in.source)
       <<",\"phase\":\""<<tx::phaseName(context.phase)<<"\",\"before_reason\":"<<unsigned(context.before)<<",\"after_reason\":"<<unsigned(context.after)
       <<",\"sequence\":"<<context.sequence<<",\"transaction_epoch\":"<<context.transactionEpoch<<",\"device\":"<<context.device
       <<",\"command\":"<<context.command<<",\"pool\":"<<context.pool<<",\"command_generation\":"<<context.commandGeneration<<",\"pool_generation\":"<<context.poolGeneration
       <<",\"draw_ordinal\":"<<context.drawOrdinal<<",\"current_subpass\":"<<context.currentSubpass<<",\"target_subpass\":"<<context.targetSubpass<<",\"subpass_count\":"<<context.subpassCount
       <<",\"rejected\":"<<(context.rejected?"true":"false")<<",\"metadata_truncated\":"<<(context.metadataTruncated?"true":"false")
       <<",\"detail_reason\":"<<in.detailReason<<",\"structure_type\":"<<in.structureType<<",\"index\":"<<in.index<<",\"chain\":"<<in.chain<<",\"contents\":"<<in.contents
       <<",\"object\":"<<in.object<<",\"flags\":"<<in.flags<<",\"target_pipeline\":"<<(in.targetPipeline?"true":"false")<<",\"chains\":[";
    for(unsigned i=0;i<in.chains.size();++i){if(i)out<<',';const auto& chain=in.chains[i];
        out<<"{\"root_type\":"<<chain.rootType<<",\"termination\":"<<chain.termination<<",\"types\":[";
        for(unsigned n=0;n<std::min<std::size_t>(chain.count,chain.types.size());++n){if(n)out<<',';out<<chain.types[n];}
        out<<"]}";}
    out<<']';
    // Finite tuple [profile, payload-observed, SRGB mask]. Omit on public
    // paths so absent private payload never looks decoded or authorized.
    if(in.privateProfile||in.privatePayloadObserved)out<<",\"private_srgb\":["<<in.privateProfile<<','<<(in.privatePayloadObserved?"true":"false")<<','<<in.privateSrgbMask<<']';
    out<<'}';
}
bool recordingContextLine(const frame_pixel_ssbo_transaction::RecordingContext& context,unsigned index,std::uint64_t count,std::uint64_t last,bool first,std::string& text) {
    std::ostringstream out;out.imbue(std::locale::classic());
    out<<"{\"event\":\""<<(first?"ssbo_recording_first_refusal":"ssbo_recording_context")<<"\",\"context_index\":"<<index<<",\"observed_count\":"<<count<<",\"last_sequence\":"<<last<<",\"context\":";
    recordingContextJson(out,context);out<<",\"native_frame_association_proven\":false,\"mask_enabled\":false}\n";
    text=out.str();if(text.size()>RecordingContextLineBytes){text.clear();return false;}return true;
}
void recordingMetadataJson(std::ostream& out,const frame_pixel_ssbo_transaction::RecordingContexts& contexts) {
    out<<",\"recording_context_count\":"<<contexts.count<<",\"recording_context_attempts\":"<<contexts.attempts<<",\"recording_context_overflow\":"<<contexts.overflow<<",\"recording_context_contended\":"<<(contexts.contended?"true":"false")<<",\"first_recording_refusal\":";
    if(contexts.firstRefusalAvailable)recordingContextJson(out,contexts.firstRefusal);else out<<"null";
    out<<",\"recording_context_repeats\":[";
    for(unsigned i=0;i<std::min<std::size_t>(contexts.count,contexts.entries.size());++i){if(i)out<<',';const auto& entry=contexts.entries[i];out<<'['<<i<<','<<entry.count<<','<<entry.lastSequence<<']';}out<<']';
}
struct RecordingReportProgress {unsigned contexts=0;bool firstRefusal=false,overflow=false;};
void recordingStatus(const frame_pixel_ssbo_transaction::RecordingContexts& contexts,int fd,RecordingReportProgress& reported) {
    const auto emit=[&](const auto& context,unsigned index,std::uint64_t count,std::uint64_t last,bool first){
        std::string text;if(recordingContextLine(context,index,count,last,first,text))frame_pixel_io::writeAll(fd,text.data(),text.size());
        else {constexpr char failure[]="{\"event\":\"ssbo_recording_context_reporting_failed\",\"mask_enabled\":false}\n";frame_pixel_io::writeAll(fd,failure,sizeof(failure)-1);}};
    while(reported.contexts<std::min<std::size_t>(contexts.count,contexts.entries.size())){const auto index=reported.contexts++;const auto& entry=contexts.entries[index];emit(entry.first,index,entry.count,entry.lastSequence,false);}
    // The independently reserved first failure survives a saturated accepted
    // bank. New distinct contexts are emitted even if refusal bits stay equal.
    if(contexts.firstRefusalAvailable&&!reported.firstRefusal){reported.firstRefusal=true;emit(contexts.firstRefusal,UINT32_MAX,1,contexts.firstRefusal.sequence,true);}
    if((contexts.overflow||contexts.contended)&&!reported.overflow){reported.overflow=true;char report[256]{};
        const auto n=std::snprintf(report,sizeof(report),"{\"event\":\"ssbo_recording_context_overflow\",\"attempts\":%llu,\"overflow\":%llu,\"contended\":%s,\"mask_enabled\":false}\n",static_cast<unsigned long long>(contexts.attempts),static_cast<unsigned long long>(contexts.overflow),contexts.contended?"true":"false");
        if(n>0&&std::size_t(n)<sizeof(report))frame_pixel_io::writeAll(fd,report,std::size_t(n));}
}
void nativeSubpassFinalMetadataJson(std::ostream& out,const frame_pixel_native_driver::Status& s) {
    // Exactly one final cold snapshot; never a repeating callback/frame log.
    out<<",\"native_subpass_provider_final\":{\"state\":\""<<frame_pixel_native_driver::stateName(s.state)<<"\",\"reason\":\""<<frame_pixel_native_driver::refusalName(s.refusal)<<"\",\"epoch\":"<<s.epoch
       <<",\"acquire_misses\":"<<s.acquireMisses<<",\"active_permits\":"<<s.activePermits<<'}';
}

#endif
void save(Device& d, const frame_pixels::View& view, int progress) {
    const auto& config = session().config;
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    if (!d.uniformRecorded || !frame_uniform_metadata::compatible(
            d.candidate,d.uniformDispatch,view,d.samplerDispatch.totalBytes())) {
        status(progress,"uniform_record_refused");return;
    }
#endif
    const auto bytes = static_cast<std::size_t>(view.bytes);
    const auto yBytes = static_cast<std::size_t>(view.layout.planes[1].bufferOffset);
    const auto& dispatch=d.samplerDispatch;
    if(!d.samplerRecorded || d.sampler.state()!=frame_pixel_sampler::State::Recorded ||
        dispatch.offset()!=view.bytes || dispatch.totalBytes()>view.capacity ||
        dispatch.extent().width!=view.layout.extent.width || dispatch.extent().height!=view.layout.extent.height ||
        dispatch.sourceViewGeneration()!=d.candidate.binding.view.identity.generation) {
        status(progress,"sampler_record_refused");return;
    }
    const int raw = frame_pixel_io::create(config, "snapshot.p010");
    if (raw < 0) { status(progress, "output_refused"); return; }
    const bool written = frame_pixel_io::writeAll(raw, view.data, bytes) && !fsync(raw);
    close(raw);
    if (!written) { status(progress, "output_failed"); return; }
    const auto sampledBytes=static_cast<std::size_t>(dispatch.sampledBytes());
    const auto* sampledData=view.data+bytes;
    const int sampledOutput=frame_pixel_io::create(config,"sampler-components.rgba8");
    if(sampledOutput<0){status(progress,"sampler_output_refused");return;}
    const bool sampledWritten=frame_pixel_io::writeAll(sampledOutput,sampledData,sampledBytes) && !fsync(sampledOutput);
    close(sampledOutput);if(!sampledWritten){status(progress,"sampler_output_failed");return;}
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    const int uniformOutput=frame_pixel_io::create(config,"uniform-bytes.bin");
    if(uniformOutput<0){status(progress,"uniform_output_refused");return;}
    const bool uniformWritten=frame_pixel_io::writeAll(uniformOutput,view.data+d.uniformDispatch.offset(),
        static_cast<std::size_t>(d.uniformDispatch.bytes())) && !fsync(uniformOutput);
    close(uniformOutput);if(!uniformWritten){status(progress,"uniform_output_failed");return;}
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    frame_pixel_ssbo_transaction::RecordView invocation;
    const bool ssboReady=d.bridge.publish(view,invocation);
    if(ssboReady){
        const int output=frame_pixel_io::create(config,"ssbo-original-invocation.bin");
        if(output<0){status(progress,"ssbo_output_refused");return;}
        const bool written=invocation.bytes==128&&frame_pixel_io::writeAll(output,invocation.record,128)&&!fsync(output);
        close(output);if(!written){status(progress,"ssbo_output_failed");return;}
    }
#endif
    const auto& b = d.candidate.binding;
    const auto& conversion = b.view.conversion;
    std::ostringstream record;
    record << "{\"schema\":\"frame-decoded-p010-snapshot-v1\",\"format\":" << int(view.layout.format)
        << ",\"width\":" << view.layout.extent.width << ",\"height\":" << view.layout.extent.height
        << ",\"byte_length\":" << bytes << ",\"sha256\":\"" << frame_pixel_io::digest(view.data,bytes)
        << "\",\"immutable\":true,\"generation\":{\"capture_id\":\"" << config.captureId
        << "\",\"image_generation\":" << view.sourceGeneration << ",\"acquisition_id\":1,\"conversion_generation\":"
        << conversion.identity.generation << "},\"planes\":[{\"name\":\"Y\",\"offset\":0,\"length\":" << yBytes
        << ",\"sha256\":\"" << frame_pixel_io::digest(view.data,yBytes) << "\"},{\"name\":\"UV\",\"offset\":"
        << yBytes << ",\"length\":" << bytes-yBytes << ",\"sha256\":\""
        << frame_pixel_io::digest(view.data+yBytes,bytes-yBytes) << "\"}],\"conversion\":";
    if (conversion.complete && conversion.model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY &&
        conversion.range == VK_SAMPLER_YCBCR_RANGE_ITU_FULL &&
        conversion.components.r == VK_COMPONENT_SWIZZLE_IDENTITY && conversion.components.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
        conversion.components.b == VK_COMPONENT_SWIZZLE_IDENTITY && conversion.components.a == VK_COMPONENT_SWIZZLE_IDENTITY &&
        (conversion.xChromaOffset==VK_CHROMA_LOCATION_MIDPOINT || conversion.xChromaOffset==VK_CHROMA_LOCATION_COSITED_EVEN) &&
        (conversion.yChromaOffset==VK_CHROMA_LOCATION_MIDPOINT || conversion.yChromaOffset==VK_CHROMA_LOCATION_COSITED_EVEN) &&
        (conversion.chromaFilter==VK_FILTER_LINEAR || conversion.chromaFilter==VK_FILTER_NEAREST)) {
        record << "{\"model\":\"rgb_identity\",\"range\":\"itu_full\",\"components\":{\"r\":\"r\",\"g\":\"g\",\"b\":\"b\",\"a\":\"a\"},"
               << "\"x_chroma_offset\":\"" << (conversion.xChromaOffset == VK_CHROMA_LOCATION_MIDPOINT ? "midpoint" : "cosited_even")
               << "\",\"y_chroma_offset\":\"" << (conversion.yChromaOffset == VK_CHROMA_LOCATION_MIDPOINT ? "midpoint" : "cosited_even")
               << "\",\"chroma_filter\":\"" << (conversion.chromaFilter == VK_FILTER_LINEAR ? "linear" : "nearest") << "\"}";
    } else record << "null";
    record << ",\"observed_vulkan_conversion\":{\"complete\":" << (conversion.complete ? "true" : "false")
        << ",\"has_view_conversion\":" << (b.view.extensions.hasConversion?"true":"false")
        << ",\"format\":" << int(conversion.format) << ",\"force_explicit_reconstruction\":" << int(conversion.forceExplicitReconstruction)
        << ",\"model\":" << int(conversion.model) << ",\"range\":" << int(conversion.range)
        << ",\"components\":[" << int(conversion.components.r) << ',' << int(conversion.components.g) << ','
        << int(conversion.components.b) << ',' << int(conversion.components.a) << "],\"chroma_offsets\":["
        << int(conversion.xChromaOffset) << ',' << int(conversion.yChromaOffset) << "],\"chroma_filter\":" << int(conversion.chromaFilter)
        << "},\"acquisition\":{\"path\":\"same_primary_after_distortion_renderpass_gpu_plane_copy\",\"owned_fence_complete\":true,"
        << "\"one_time_submit\":" << (view.oneTimeSubmit ? "true" : "false")
        << ",\"retirement_required\":" << (view.retirementRequired ? "true" : "false")
        << ",\"generation_retired\":" << (view.generationRetired ? "true" : "false")
        << ",\"command_generation\":" << view.commandGeneration << ",\"view_generation\":"
        << b.view.identity.generation << ",\"queue_family\":" << d.candidate.queueFamily
        << ",\"source_descriptor_type\":" << unsigned(b.type)
        << ",\"source_descriptor_count\":" << b.descriptorCount
        << ",\"source_descriptor_stages\":" << b.stages
        << "},\"sampler_control\":{\"schema\":\"frame-decoded-p010-sampler-control-v1\","
           "\"file\":\"sampler-components.rgba8\",\"capture_id\":\"" << config.captureId
        << "\",\"image_generation\":" << view.sourceGeneration
        << ",\"conversion_generation\":" << conversion.identity.generation
        << ",\"command_generation\":" << view.commandGeneration
        << ",\"view_generation\":" << b.view.identity.generation
        << ",\"width\":" << view.layout.extent.width << ",\"height\":" << view.layout.extent.height
        << ",\"byte_length\":" << sampledBytes
        << ",\"sha256\":\"" << frame_pixel_io::digest(sampledData,sampledBytes)
        << "\",\"encoding\":\"RGBA8_UNORM sampled RGB_IDENTITY components; not reconstructed scene RGB\","
           "\"sample_positions\":\"normalized logical texel centers, explicit LOD zero\","
           "\"same_command_generation\":true,\"same_image_view_generation\":true,\"graphics_state_untouched\":true,"
           "\"no_prior_compute_state\":" << (d.candidate.noPriorComputeState?"true":"false")
        << ",\"shared_storage_capacity\":" << view.capacity << ",\"buffer_offset\":" << dispatch.offset()
        << ",\"total_paired_bytes\":" << dispatch.totalBytes() << "},\"native_image_layout\":{\"tiling\":" << int(b.image.tiling)
        << ",\"flags\":" << b.image.flags << ",\"usage\":" << b.image.usage
        << ",\"sharing_mode\":" << int(b.image.sharingMode) << ",\"allocation_bytes\":" << b.image.allocation.allocationSize
        << ",\"explicit_drm_modifier\":" << (b.image.extensions.hasDrmExplicit?"true":"false")
        << ",\"drm_modifier\":" << b.image.extensions.drmModifier << ",\"memory_planes\":[";
    for(std::uint32_t n=0;n<b.image.extensions.planeCount && n<b.image.extensions.planes.size();++n) {
        if(n)record<<',';
        const auto& plane=b.image.extensions.planes[n];
        record<<"{\"offset\":"<<plane.offset<<",\"row_pitch\":"<<plane.rowPitch
              <<",\"array_pitch\":"<<plane.arrayPitch<<",\"depth_pitch\":"<<plane.depthPitch<<'}';
    }
    record<<"]}";
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    if(!frame_uniform_metadata::append(record,d.candidate,d.uniformDispatch,view,
        d.samplerDispatch.totalBytes(),config.captureId)){status(progress,"uniform_metadata_refused");return;}
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto ssbo=d.bridge.snapshot();
    record<<",\"native_ssbo_control\":{\"available\":"<<(ssboReady?"true":"false")
          <<",\"reason\":\""<<frame_pixel_ssbo_receiver::refusalName(ssbo.refusal)<<"\",\"transaction_reason\":"<<ssbo.transaction
          <<",\"draw_kind\":"<<ssbo.drawKind<<",\"draw_ordinal\":"<<ssbo.drawOrdinal<<",\"draw_args\":["<<ssbo.drawArgs[0]<<','<<ssbo.drawArgs[1]<<','<<ssbo.drawArgs[2]<<','<<ssbo.drawArgs[3]<<','<<ssbo.drawArgs[4]<<']'
          <<",\"submit_route\":"<<ssbo.submitRoute<<",\"submit_batches\":"<<ssbo.submit.batches<<",\"submit_batch\":"<<ssbo.submit.batch<<",\"submit_commands\":"<<ssbo.submit.commandCount
          <<",\"submit_next_type\":"<<ssbo.submit.nextType<<",\"submit_occurrences\":"<<ssbo.submit.selectedOccurrences<<",\"pool_generation\":"<<ssbo.poolGeneration
          <<",\"source_executions\":"<<ssbo.sourceExecutions<<",\"paired_execution\":"<<ssbo.pairedExecution
          <<",\"original_color_expression_preserved\":true,\"native_visual_result_verified\":false,\"sparse_grid32_invocation\":true";
    recordingMetadataJson(record,ssbo.recordingContexts);
    if(ssboReady)record<<",\"file\":\"ssbo-original-invocation.bin\",\"bytes\":128,\"sha256\":\""
          <<frame_pixel_io::digest(static_cast<const std::uint8_t*>(invocation.record),128)<<"\",\"command_generation\":"<<invocation.commandGeneration
          <<",\"execution\":"<<invocation.execution<<",\"source_cohort\":{\"device\":"<<invocation.source.image.device<<",\"layout\":"<<unsigned(invocation.source.layout)<<",\"extent\":["<<invocation.source.extent.width<<','<<invocation.source.extent.height<<"],\"image\":"<<invocation.source.image.value<<",\"image_generation\":"<<invocation.source.image.generation
          <<",\"view\":"<<invocation.source.view.value<<",\"view_generation\":"<<invocation.source.view.generation<<",\"memory\":"<<invocation.source.memory.value<<",\"memory_generation\":"<<invocation.source.memory.generation<<'}';
    record<<",\"native_frame_association_proven\":false,\"physical_eye_proven\":false,\"passthrough_authorized\":false}";
    nativeSubpassFinalMetadataJson(record,d.nativeSubpassLease.status());
#endif
    record<<",\"source_frame_associated\":false,\"uv_owned\":false,\"passthrough_authorized\":false}\n";
    const int meta = frame_pixel_io::create(config, "snapshot.json");
    if (meta < 0) { status(progress, "metadata_refused"); return; }
    const auto text = record.str();
    if(text.size()>16384){close(meta);status(progress,"metadata_extent_refused");return;}
    const bool good = frame_pixel_io::writeAll(meta,text.data(),text.size()) && !fsync(meta);
    close(meta); status(progress,good ? "complete" : "metadata_failed");
}
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
void definitionStatus(Device& d,int fd,bool& featureReported,std::uint64_t& reportedReasons,unsigned& reportedRecipes,
                      unsigned& reportedSets,std::uint32_t& reportedSetSkips,unsigned& reportedContexts,unsigned& reportedCreations,
                      unsigned& reportedGraphics,bool& reportedOverflow) {
    namespace journal=frame_pixel_ssbo_journal;
    static_assert(unsigned(journal::Reason::Count)<63,"bit63 is the sticky observer-failure receipt");
    if(d.definitionBoundary.failed()){
        if(!(reportedReasons&(UINT64_C(1)<<63))){
            constexpr char message[]="{\"event\":\"ssbo_definition_observation_failed\",\"pipeline_changed\":false,\"mask_enabled\":false}\n";
            frame_pixel_io::writeAll(fd,message,sizeof(message)-1);reportedReasons|=UINT64_C(1)<<63;
        }
        return;
    }
    const auto facts=d.definitions.inspectStats();
    const auto observed=d.definitions.inspectFacts();
    if(!featureReported){
        char report[768]{};
        const auto count=std::snprintf(report,sizeof(report),
            "{\"event\":\"ssbo_definition_feature\",\"initialized\":%s,\"observed\":%s,\"enabled\":%s,\"raw\":%u,"
            "\"source\":%u,\"contract_reason\":\"%s\",\"contract_index\":%u,\"contract_structure_type\":%u,"
            "\"native_compatibility_proven\":false,\"mask_enabled\":false}\n",
            facts.initialized?"true":"false",facts.feature.observed?"true":"false",facts.feature.enabled?"true":"false",
            facts.feature.raw,unsigned(facts.feature.source),frame_pixel_ssbo_clone::reasonName(facts.deviceRefusal.reason),
            facts.deviceRefusal.index,facts.deviceRefusal.type);
        if(count>0 && static_cast<std::size_t>(count)<sizeof(report))frame_pixel_io::writeAll(fd,report,static_cast<std::size_t>(count));
        // Report every bounded header together, even when clone admission stops
        // at the first unknown extension. This is observation, never a bypass.
        std::ostringstream shape;
        shape<<"{\"event\":\"ssbo_definition_device_shape\",\"observed\":"<<(observed.deviceObserved?"true":"false")
             <<",\"chain_end\":\""<<frame_pixel_definition_facts::chainEndName(observed.chainEnd)<<"\",\"chain_types\":[";
        for(std::uint32_t i=0;i<observed.chainCount;++i){if(i)shape<<',';shape<<observed.chainTypes[i];}
        shape<<"],\"clone_admission_changed\":false,\"mask_enabled\":false}\n";
        const auto text=shape.str();
        if(text.size()<=1024)frame_pixel_io::writeAll(fd,text.data(),text.size());
        featureReported=true;
    }
    while(reportedSets<observed.setCount){
        const auto index=reportedSets++;const auto& set=observed.sets[index];std::ostringstream shape;
        shape<<"{\"event\":\"ssbo_definition_set_shape\",\"shape_index\":"<<index<<",\"bindings\":[";
        for(std::uint32_t i=0;i<set.count;++i){if(i)shape<<',';const auto& binding=set.bindings[i];
            shape<<"{\"binding\":"<<binding.binding<<",\"type\":"<<binding.type<<",\"count\":"<<binding.count
                 <<",\"stages\":"<<binding.stages<<",\"immutable_sampler_present\":"<<(binding.immutableSampler?"true":"false")<<'}';
        }
        shape<<"],\"native_pipeline_association_proven\":false,\"clone_admission_changed\":false,\"mask_enabled\":false}\n";
        const auto text=shape.str();
        if(text.size()<=8192)frame_pixel_io::writeAll(fd,text.data(),text.size());
    }
    const auto newSkips=observed.setSkipReasons&~reportedSetSkips;
    if(newSkips){
        char report[256]{};const auto count=std::snprintf(report,sizeof(report),
            "{\"event\":\"ssbo_definition_set_shape_skipped\",\"new_reasons\":%u,\"mask_enabled\":false}\n",newSkips);
        if(count>0 && static_cast<std::size_t>(count)<sizeof(report))frame_pixel_io::writeAll(fd,report,static_cast<std::size_t>(count));
        reportedSetSkips|=newSkips;
    }
    while(reportedCreations<observed.creationCount){
        const auto index=reportedCreations++;const auto& item=observed.creations[index];const auto& shape=item.shape;std::ostringstream out;
        out<<"{\"event\":\"ssbo_definition_creation_shape\",\"shape_index\":"<<index<<",\"kind\":"<<unsigned(shape.kind)<<",\"route\":"<<shape.route<<",\"flags\":"<<shape.flags<<",\"counts\":["<<shape.counts[0]<<','<<shape.counts[1]<<','<<shape.counts[2]<<','<<shape.counts[3]<<"],\"base_index\":"<<shape.baseIndex<<",\"base_handle_present\":"<<(shape.baseHandle?"true":"false")
           <<",\"first_object\":"<<item.firstObject<<",\"last_object\":"<<item.lastObject<<",\"first_layout\":"<<item.firstLayout<<",\"last_layout\":"<<item.lastLayout<<",\"first_pass\":"<<item.firstPass<<",\"last_pass\":"<<item.lastPass<<",\"count\":"<<item.count<<",\"chain_end\":\""<<frame_pixel_definition_facts::chainEndName(shape.chainEnd)<<"\",\"chain_types\":[";
        for(std::uint32_t n=0;n<shape.chainCount;++n){if(n)out<<',';out<<shape.chainTypes[n];}
        out<<"],\"admission_proven\":false,\"native_frame_association_proven\":false,\"mask_enabled\":false}\n";
        const auto text=out.str();if(text.size()<=2048)frame_pixel_io::writeAll(fd,text.data(),text.size());
    }
    while(reportedGraphics<observed.graphicsCount){
        const auto index=reportedGraphics++;std::string text;
        if(frame_pixel_definition_report::graphics(observed.graphics[index],index,text))
            frame_pixel_io::writeAll(fd,text.data(),text.size());
        else{
            constexpr char failure[]="{\"event\":\"ssbo_definition_graphics_reporting_failed\",\"mask_enabled\":false}\n";
            frame_pixel_io::writeAll(fd,failure,sizeof(failure)-1);
        }
    }
    auto newReasons=facts.seenReasons&~reportedReasons;
    const bool overflow=observed.creationOverflow||observed.graphicsOverflow||facts.contextOverflow;
    // New keyed contexts are grouped, never one report per native create or
    // repeated failure. NoReady cannot hide later Contract/Layout evidence.
    while(newReasons||reportedContexts<facts.contextCount||(overflow&&!reportedOverflow)){
        const auto stop=std::min<unsigned>(facts.contextCount,reportedContexts+8);std::ostringstream out;
        out<<"{\"event\":\"ssbo_definition_refusals\",\"new_reasons\":"<<newReasons<<",\"first_reason\":\""<<journal::reasonName(facts.firstRefusal.reason)<<"\",\"first_contract_reason\":\""<<frame_pixel_ssbo_clone::reasonName(facts.firstRefusal.contract.reason)<<"\",\"contexts\":[";
        for(unsigned index=reportedContexts;index<stop;++index){const auto& context=facts.contexts[index];const auto& first=context.first;const auto& last=context.last;
            if(index!=reportedContexts)out<<',';
            out<<"{\"context_index\":"<<index<<",\"reason\":\""<<journal::reasonName(first.reason)<<"\",\"kind\":"<<unsigned(first.kind)<<",\"contract_reason\":\""<<frame_pixel_ssbo_clone::reasonName(first.contract.reason)<<"\",\"contract_index\":"<<first.contract.index<<",\"contract_structure_type\":"<<first.contract.type<<",\"first_object\":"<<first.value<<",\"first_generation\":"<<first.generation<<",\"last_object\":"<<last.value<<",\"last_generation\":"<<last.generation<<",\"count\":"<<context.count<<'}';
        }
        out<<"],\"inventory\":[";for(unsigned kind=0;kind<6;++kind){if(kind)out<<',';out<<"{\"kind\":"<<kind<<",\"live\":"<<facts.live[kind]<<",\"capacity\":"<<facts.capacity[kind]<<'}';}
        out<<"],\"set_fact_count\":"<<facts.setFactCount<<",\"set_fact_capacity\":"<<facts.setFactCapacity<<",\"context_overflow\":"<<facts.contextOverflow<<",\"creation_shape_overflow\":"<<(observed.creationOverflow?"true":"false")<<",\"graphics_observation_overflow\":"<<(observed.graphicsOverflow?"true":"false")<<",\"created\":"<<facts.created<<",\"cancelled\":"<<facts.cancelled<<",\"native_frame_association_proven\":false,\"mask_enabled\":false}\n";
        const auto text=out.str();if(text.size()<=8192)frame_pixel_io::writeAll(fd,text.data(),text.size());
        reportedContexts=stop;reportedReasons|=newReasons;newReasons=0;reportedOverflow=overflow;
    }
    // This optional phase observes complete definitions; it does NOT create or
    // bind a clone. Four receipts are the whole process's reporting budget.
    if(reportedRecipes>=4 || !facts.feature.complete)return;
    journal::Ready ready;
    if(!d.definitions.takeReady(ready))return;
    ++reportedRecipes;
    std::ostringstream out;
    out<<"{\"event\":\"ssbo_definition_owned\",\"pipeline_generation\":"<<ready.pipeline.generation
       <<",\"current_at_cold_lookup\":"<<(d.definitions.current(ready.pipeline)?"true":"false")
       <<",\"set0_bindings\":[";
    for(std::uint32_t i=0;i<ready.recipe.set0.count;++i){const auto& binding=ready.recipe.set0.bindings[i];
        if(i)out<<',';
        out<<"{\"binding\":"<<binding.binding<<",\"type\":"<<unsigned(binding.descriptorType)
           <<",\"count\":"<<binding.descriptorCount<<",\"stages\":"<<binding.stageFlags<<'}';
    }
    const auto* selected=ready.recipe.renderPass.subpass(ready.recipe.graphics.info.subpass);
    out<<"],\"push_range_count\":"<<ready.recipe.layout.count<<",\"color_attachment_count\":"<<(selected?selected->colorCount:0)
       <<",\"subpass_count\":"<<ready.recipe.renderPass.subpassCount<<",\"selected_subpass\":"<<ready.recipe.graphics.info.subpass
       <<",\"renderpass2_route\":"<<(ready.recipe.renderPass.route2?"true":"false")
       <<",\"renderpass2_khr_route\":"<<(ready.recipe.renderPass.route2KHR?"true":"false")
       <<",\"vertex_bytes\":"<<ready.recipe.vertex.bytes<<",\"fragment_bytes\":"<<ready.recipe.fragment.bytes
       <<",\"native_lifetime_lease\":false,\"native_compatibility_proven\":false,\"pipeline_changed\":false,\"mask_enabled\":false}\n";
    const auto text=out.str();
    if(text.size()<=8192)frame_pixel_io::writeAll(fd,text.data(),text.size());
}
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
bool nativeSubpassStatusLine(const frame_pixel_native_driver::Status& s,std::string& line) {
    // Provider paths can be untrusted on refusal. Emit a bounded ASCII/JSON
    // prefix and explicit truncation, never an unescaped native module string.
    std::size_t pathBytes=0;while(pathBytes<sizeof(s.providerPath)&&s.providerPath[pathBytes])++pathBytes;
    constexpr std::size_t PathPrefix=128;char escaped[PathPrefix*6+1]{};std::size_t used=0;
    constexpr char hex[]="0123456789abcdef";
    for(std::size_t i=0;i<std::min(pathBytes,PathPrefix);++i){const auto c=static_cast<unsigned char>(s.providerPath[i]);
        if(c=='"'||c=='\\'){escaped[used++]='\\';escaped[used++]=char(c);}
        else if(c<0x20||c>0x7e){escaped[used++]='\\';escaped[used++]='u';escaped[used++]='0';escaped[used++]='0';escaped[used++]=hex[c>>4];escaped[used++]=hex[c&15];}
        else escaped[used++]=char(c);
    }
    char report[2048]{};
    const auto count=std::snprintf(report,sizeof(report),
        "{\"event\":\"native_subpass_provider\",\"state\":\"%s\",\"reason\":\"%s\",\"epoch\":%llu,\"device\":%llu,\"module_base\":%llu,\"inode\":%llu,\"device_major\":%llu,\"device_minor\":%llu,\"chip\":%u,\"route_mask\":%u,\"observed_route\":%u,\"acquire_misses\":%llu,\"entry_rvas\":[%llu,%llu,%llu,%llu],\"provider_path\":\"%s\",\"provider_path_bytes\":%zu,\"provider_path_truncated\":%s,\"native_frame_association_proven\":false,\"mask_enabled\":false}\n",
        frame_pixel_native_driver::stateName(s.state),frame_pixel_native_driver::refusalName(s.refusal),
        static_cast<unsigned long long>(s.epoch),static_cast<unsigned long long>(s.device),static_cast<unsigned long long>(s.moduleBase),static_cast<unsigned long long>(s.inode),static_cast<unsigned long long>(s.deviceMajor),static_cast<unsigned long long>(s.deviceMinor),s.chip,s.routeMask,s.observedRoute,static_cast<unsigned long long>(s.acquireMisses),
        static_cast<unsigned long long>(s.rvas[0]),static_cast<unsigned long long>(s.rvas[1]),static_cast<unsigned long long>(s.rvas[2]),static_cast<unsigned long long>(s.rvas[3]),escaped,pathBytes,pathBytes>PathPrefix?"true":"false");
    if(count<=0||std::size_t(count)>=sizeof(report)){line.clear();return false;}
    line.assign(report,std::size_t(count));return true;
}
void nativeSubpassStatus(Device& d,int fd) noexcept {
    try{std::string line;if(nativeSubpassStatusLine(d.nativeSubpassLease.status(),line))frame_pixel_io::writeAll(fd,line.data(),line.size());}
    catch(...){constexpr char failure[]="{\"event\":\"native_subpass_provider_reporting_failed\",\"mask_enabled\":false}\n";frame_pixel_io::writeAll(fd,failure,sizeof(failure)-1);}
}
void bridgeStatus(Device& d,int fd,std::uint64_t& seen,RecordingReportProgress& recordingReported) {
    const auto s=d.bridge.snapshot();recordingStatus(s.recordingContexts,fd,recordingReported);
    const std::uint64_t state=1u|std::uint64_t(s.profile)<<1|std::uint64_t(s.ready)<<2|std::uint64_t(s.learned)<<3|
        std::uint64_t(s.adopted)<<4|std::uint64_t(s.replaced)<<5|std::uint64_t(s.copy)<<6|std::uint64_t(s.queueKnown)<<7|std::uint64_t(s.refusal)<<8|
        std::uint64_t(s.transaction)<<16|std::uint64_t(s.cloneStatus)<<24|std::uint64_t(s.unmappedCandidate)<<32;
    if(seen==state)return;
    seen=state;
    char report[2048]{};const auto n=std::snprintf(report,sizeof(report),
        "{\"event\":\"ssbo_transaction_state\",\"reason\":\"%s\",\"transaction_reason\":%u,"
        "\"profile\":%s,\"ready\":%s,\"learned\":%s,\"adopted\":%s,\"replaced\":%s,\"copy\":%s,"
        "\"queue_create_infos\":%u,\"queue_count\":%u,\"queue_family\":%u,\"queue_retrieved\":%s,\"retrieved_family\":%u,\"retrieved_index\":%u,\"retrieved_queue\":%llu,\"clone_status\":%u,\"clone_stage\":%u,"
        "\"clone_vk_result\":%d,\"draw_kind\":%u,\"draw_ordinal\":%llu,\"draw_args\":[%u,%u,%u,%u,%u],"
        "\"submit_route\":%u,\"submit_batches\":%u,\"submit_batch\":%u,\"submit_commands\":%u,\"submit_next_type\":%u,\"submit_occurrences\":%u,"
        "\"command_generation\":%llu,\"pool_generation\":%llu,\"selected_pipeline\":%llu,\"selected_pipeline_generation\":%llu,\"learned_command_generation\":%llu,\"learned_pool_generation\":%llu,\"unmapped_candidate\":%s,\"source_executions\":%llu,\"paired_execution\":%llu,\"original_color_expression_preserved\":true,\"native_visual_result_verified\":false,\"native_frame_association_proven\":false,\"mask_enabled\":false}\n",
        frame_pixel_ssbo_receiver::refusalName(s.refusal),s.transaction,s.profile?"true":"false",s.ready?"true":"false",
        s.learned?"true":"false",s.adopted?"true":"false",s.replaced?"true":"false",s.copy?"true":"false",
        s.queueCreateInfos,s.queueCount,s.queueFamily,s.queueKnown?"true":"false",s.retrievedFamily,s.retrievedIndex,static_cast<unsigned long long>(s.retrievedQueue),s.cloneStatus,s.cloneStage,s.cloneDriver,s.drawKind,static_cast<unsigned long long>(s.drawOrdinal),
        s.drawArgs[0],s.drawArgs[1],s.drawArgs[2],s.drawArgs[3],s.drawArgs[4],s.submitRoute,s.submit.batches,s.submit.batch,s.submit.commandCount,s.submit.nextType,s.submit.selectedOccurrences,
        static_cast<unsigned long long>(s.commandGeneration),static_cast<unsigned long long>(s.poolGeneration),static_cast<unsigned long long>(s.selectedPipeline.value),static_cast<unsigned long long>(s.selectedPipeline.generation),static_cast<unsigned long long>(s.learnedCommandGeneration),static_cast<unsigned long long>(s.learnedPoolGeneration),s.unmappedCandidate?"true":"false",static_cast<unsigned long long>(s.sourceExecutions),static_cast<unsigned long long>(s.pairedExecution));
    if(n>0&&std::size_t(n)<sizeof(report))frame_pixel_io::writeAll(fd,report,std::size_t(n));
}
#endif
void preparationStatus(Device* d,int progress) noexcept {
    // One cold background record. It must explain a refused preparation before
    // asking the maintainer to start a stream; never emit per-frame diagnostics.
    char preparation[4096]{};
    const auto preparedCount=std::snprintf(preparation,sizeof(preparation),
        "{\"event\":\"prepared\",\"vk_result\":0,\"sampler_ready\":%s,\"sampler_vk_result\":%d,\"sampler_prepare_attempted\":%s,"
        "\"device_admission_complete\":%s,\"ycbcr_enabled\":%s,\"compute_routes_observed\":%s,"
        "\"ycbcr_feature_observed\":%s,\"query_routes_observed\":%s,\"device_admission_reason\":\"%s\","
        "\"offending_index\":%u,\"offending_structure_type\":%u,\"offending_loader_function\":%u,"
        "\"pipeline_statistics_query_observed\":%s,\"pipeline_statistics_query_enabled\":%s,"
        "\"pipeline_statistics_query_source\":%u,\"pipeline_statistics_query_value\":%u,"
        "\"instance_api_version\":%u,\"native_device\":%llu,\"unreviewed_extension\":\"%s\",\"format_queries_complete\":%s,"
        "\"format_routes\":%u,\"pool_descriptor_count\":%u,\"native_compute_queue\":%s,"
        "\"uniform_companion_enabled\":%s,\"uniform_companion_ready\":%s,"
        "\"mask_enabled\":false,\"native_frame_association_proven\":false}\n",
        d->sampler.state()==frame_pixel_sampler::State::Ready?"true":"false",int(d->samplerPrepareResult),
        d->samplerPrepareAttempted?"true":"false",
        d->samplerEligibility.complete?"true":"false",d->samplerEligibility.samplerYcbcrConversionEnabled?"true":"false",
        d->samplerEligibility.computeRoutesObserved?"true":"false",
        d->samplerEligibility.samplerYcbcrConversionObserved?"true":"false",d->samplerEligibility.queryRoutesObserved?"true":"false",
        frame_pixel_sampler::refusalName(d->samplerEligibility.refusal),d->samplerEligibility.offendingIndex,
        d->samplerEligibility.offendingStructureType,d->samplerEligibility.offendingLoaderFunction,
        d->samplerEligibility.pipelineStatisticsObserved?"true":"false",d->samplerEligibility.pipelineStatisticsEnabled?"true":"false",
        unsigned(d->samplerEligibility.pipelineStatisticsSource),d->samplerEligibility.pipelineStatisticsValue,
        d->instanceApiVersion,static_cast<unsigned long long>(frame_bindings::handle(d->native)),d->unreviewedExtension,
        d->samplerFormats.complete?"true":"false",d->samplerFormats.routeCount,d->samplerFormats.poolDescriptorCount,d->nativeComputeQueue?"true":"false",
        uniformCompanionEnabled()?"true":"false",uniformCompanionReady(*d)?"true":"false");
    if(preparedCount>0 && static_cast<std::size_t>(preparedCount)<sizeof(preparation))
        frame_pixel_io::writeAll(progress,preparation,static_cast<std::size_t>(preparedCount));
}
void writer(Device* d) noexcept {
    const int progress = frame_pixel_io::create(session().config,"status.jsonl");
    if (progress < 0 || flock(progress,LOCK_EX|LOCK_NB)) { if(progress>=0)close(progress); return; }
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    // One cold verification, before manual arming. Never resolve a provider,
    // hash an ELF, open a file or acquire its loader lease inside Begin/Next.
    d->nativeSubpassLease.prepareCold(d->nativeSubpassInputs);
    nativeSubpassStatus(*d,progress);
#endif
    preparationStatus(d,progress);
    bool announced = false, refusalAnnounced = false;
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    bool uniformRefusalAnnounced = false;
#endif
    unsigned announcedQueryRoutes=0;
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    std::uint64_t bridgeState=0;RecordingReportProgress recordingReported;
#endif
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    bool definitionFeatureReported=false;std::uint64_t definitionReasons=0;unsigned definitionRecipes=0,definitionSets=0;
    std::uint32_t definitionSetSkips=0;unsigned definitionContexts=0,definitionCreations=0,definitionGraphics=0;bool definitionOverflow=false;
#endif
    try { while (!d->stop.load(std::memory_order_acquire)) {
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
        definitionStatus(*d,progress,definitionFeatureReported,definitionReasons,definitionRecipes,definitionSets,definitionSetSkips,definitionContexts,definitionCreations,definitionGraphics,definitionOverflow);
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
        d->bridge.tick(d->definitions);
        bridgeStatus(*d,progress,bridgeState,recordingReported);
#endif
        const auto queryRoutes=d->queryRoutesUsed.load(std::memory_order_relaxed);
        if(queryRoutes!=announcedQueryRoutes) {
            // At most two observations per device, even under continuous native
            // queries. Formatting and I/O remain on this background thread.
            char report[256]{};
            const auto count=std::snprintf(report,sizeof(report),
                "{\"event\":\"sampler_query_history_observed\",\"query_routes_used\":%u,\"prior_query_blocks_sampling\":true,"
                "\"mask_enabled\":false,\"native_frame_association_proven\":false}\n",queryRoutes);
            if(count>0 && static_cast<std::size_t>(count)<sizeof(report))
                frame_pixel_io::writeAll(progress,report,static_cast<std::size_t>(count));
            announcedQueryRoutes=queryRoutes;
        }
        if (!session().armed.load(std::memory_order_acquire) && frame_pixel_io::armed(session().config)) {
            session().armed.store(true,std::memory_order_release); status(progress,"armed");
        }
        if (d->selected.load(std::memory_order_acquire)) {
            if (!announced) { status(progress,"commands_recorded"); announced=true; }
            frame_pixels::View view;
            const auto result = d->readback.poll(view);
            if (result == frame_pixels::Status::Complete) {
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
                const auto s=d->bridge.snapshot();frame_pixel_ssbo_transaction::RecordView observed;
                if(s.copy&&s.replaced&&s.refusal==frame_pixel_ssbo_receiver::Refusal::None&&!s.transaction&&!d->bridge.publish(view,observed)){
                    const auto after=d->bridge.snapshot();
                    if(after.refusal==frame_pixel_ssbo_receiver::Refusal::None&&!after.transaction){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
                }
#endif
                save(*d,view,progress);break;
            }
            if (result == frame_pixels::Status::VulkanFailure) { status(progress,"gpu_failure",d->readback.lastResult()); break; }
        }
        const auto samplerRefusal=d->samplerRefusal.load(std::memory_order_relaxed);
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
        const auto uniformRefusal=d->uniformRuntimeRefusal.load(std::memory_order_acquire);
        if(!uniformRefusalAnnounced && uniformRefusal && uniformRefusal!=UINT32_MAX) {
            const auto tracker=d->uniformRefusal.load(std::memory_order_relaxed);
            const auto resources=d->uniformResourceRefusal.load(std::memory_order_relaxed);
            char report[768]{};
            const auto count=std::snprintf(report,sizeof(report),
                "{\"event\":\"uniform_candidate_refused\",\"runtime_reason\":%u,\"tracker_reason\":%u,"
                "\"tracker_reason_name\":\"%s\",\"contract_reason\":%u,\"resource_reason\":%u,"
                "\"offending_binding\":%u,\"mask_enabled\":false,\"native_frame_association_proven\":false}\n",
                uniformRefusal,tracker,
                frame_pixel_capture::uniformRefusalName(static_cast<frame_pixel_capture::UniformRefusal>(tracker)),
                d->uniformContractRefusal.load(std::memory_order_relaxed),resources,
                d->uniformOffendingBinding.load(std::memory_order_relaxed));
            if(count>0 && static_cast<std::size_t>(count)<sizeof(report))
                frame_pixel_io::writeAll(progress,report,static_cast<std::size_t>(count));
            uniformRefusalAnnounced=true;
        }
#endif
        if(!refusalAnnounced && samplerRefusal!=SamplerRefusal::None) {
            char report[256]{};
            const auto count=std::snprintf(report,sizeof(report),
                "{\"event\":\"sampler_admission_refused\",\"reason\":\"%s\",\"mask_enabled\":false}\n",
                refusalName(samplerRefusal));
            if(count>0 && static_cast<std::size_t>(count)<sizeof(report))
                frame_pixel_io::writeAll(progress,report,static_cast<std::size_t>(count));
            refusalAnnounced=true; // First observation only, not a permanent native-path failure.
        }
        if (!d->selected.load(std::memory_order_acquire) && session().tracker->poisoned()) {
            // The first failing API freezes these counters. Publication can
            // briefly follow poisoned(), so retry on this background thread
            // rather than replace the report with an uninformative refusal.
            const auto refusal = session().tracker->failureSnapshot();
            if (refusal.complete) {
                char bytes[frame_pixel_status::MaximumRefusalBytes]{};
                const auto count = frame_pixel_status::trackingRefusal(bytes, sizeof(bytes), refusal,
                    d->selected.load(std::memory_order_acquire), session().armed.load(std::memory_order_acquire));
                if (count) frame_pixel_io::writeAll(progress, bytes, count);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Background polling only; never restarts anything.
    }} catch(...) { status(progress,"writer_failed"); }
    fsync(progress); close(progress);
}
// Capture the OLD live generation before native reset/free can replace or
// recycle handles. A later equal raw handle is never retirement evidence.
RetirementTicket retiring(Device* d, VkCommandBuffer command, VkCommandPool pool = VK_NULL_HANDLE) {
    if (!d->selected.load(std::memory_order_acquire) || command != d->candidate.command) return {};
    const auto before = session().tracker->prepareRetirement(d->id,command);
    const auto& chosen = d->candidate;
    if (!frame_pixel_selection::matchesRetirement(chosen,before) ||
        (pool && (pool != before.pool || !session().tracker->ownsCommandPool(d->id,command,pool)))) return {};
    return before;
}
void retired(Device* d, const RetirementTicket& before, bool nativeSuccess) {
    if (before.eligible) d->readback.retireCommand(before.command,before.commandGeneration,nativeSuccess);
}
void afterRenderPass(Device* d, VkCommandBuffer command, bool dynamic) {
    if (!session().allowed) return;
    Candidate candidate;
    const bool ready = dynamic ? session().tracker->endRendering(d->id,command,candidate)
                               : session().tracker->endRenderPass(d->id,command,candidate);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(ready&&session().armed.load(std::memory_order_acquire))d->bridge.learn(candidate);
#endif
    if (!ready || !d->ownsCapture || d->selected.load(std::memory_order_acquire) ||
        !session().armed.load(std::memory_order_acquire)) return;
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(d->bridge.deferCopy(candidate))return;
#endif
    const auto& image = candidate.binding.image;
    const auto refuse=[&](SamplerRefusal reason) noexcept {
        auto empty=SamplerRefusal::None;
        d->samplerRefusal.compare_exchange_strong(empty,reason,std::memory_order_relaxed);
    };
    // Updating our own cold descriptor and appending COMPUTE must never replace
    // native compute state. Known unsupported extension routes disable only this
    // experiment, and the actual command history/queue are checked at admission.
    if(!d->samplerEligibility.eligible()){refuse(SamplerRefusal::Device);return;}
    if(!candidate.noPriorComputeState){refuse(SamplerRefusal::ComputeState);return;}
    if(candidate.queueFamily>=d->queueFamilyCount ||
       !frame_pixel_sampler::queueSupportsCompute(d->queueFlags[candidate.queueFamily])){refuse(SamplerRefusal::Queue);return;}
    if(!frame_pixel_sampler_format::admitSource(d->samplerFormats,candidate.binding)){refuse(SamplerRefusal::Format);return;}
    // The offline recovery/PNG reader has the same independent 12M-pixel bound.
    // Reserve 64 MiB cold, but never save an extent that its reader cannot use.
    if (image.extent.width>8192 || image.extent.height>8192 ||
        std::uint64_t(image.extent.width)*image.extent.height>12000000){refuse(SamplerRefusal::Extent);return;}
    const auto& view=candidate.binding.view;
    const auto& conversion=view.conversion;
    frame_pixel_sampler::Source sampled;
    sampled.complete=candidate.binding.complete;
    sampled.view=reinterpret_cast<VkImageView>(static_cast<std::uintptr_t>(view.identity.handle));
    sampled.viewGeneration=view.identity.generation;sampled.extent={image.extent.width,image.extent.height};
    sampled.viewType=view.type;sampled.format=view.format;sampled.components=view.components;
    sampled.subresource=view.subresource;sampled.layout=candidate.binding.imageLayout;
    sampled.hasViewConversion=view.extensions.hasConversion;sampled.conversionComplete=conversion.complete;
    sampled.conversion=frame_pixel_sampler::SamplerProbe::profile();
    sampled.conversion.format=conversion.format;sampled.conversion.ycbcrModel=conversion.model;
    sampled.conversion.ycbcrRange=conversion.range;sampled.conversion.components=conversion.components;
    sampled.conversion.xChromaOffset=conversion.xChromaOffset;sampled.conversion.yChromaOffset=conversion.yChromaOffset;
    sampled.conversion.chromaFilter=conversion.chromaFilter;sampled.conversion.forceExplicitReconstruction=conversion.forceExplicitReconstruction;
    if (!conversion.extensions.chain.complete || conversion.extensions.chain.count){refuse(SamplerRefusal::Conversion);return;}
    frame_pixel_sampler::Dispatch dispatch;
    if(!d->sampler.admit(sampled,dispatch)){refuse(SamplerRefusal::Profile);return;} // Pure; no descriptor/GPU mutation.
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    if(!d->uniformPrepared){uniformRefuse(*d,UniformRuntimeRefusal::ColdPreparation);return;}
    if(!candidate.uniformFinalDraw || !candidate.uniformPlan.metadataCandidate) {
        uniformRefuse(*d,UniformRuntimeRefusal::LastDrawMetadata,
            static_cast<unsigned>(candidate.uniformRefusal),static_cast<unsigned>(candidate.uniformResourceRefusal.reason),
            candidate.uniformOffendingBinding,static_cast<unsigned>(candidate.uniformContractRefusal));
        return;
    }
    std::array<frame_uniform_copy::Slice,5> uniformSources{};
    for(std::size_t n=0;n<uniformSources.size();++n) {
        const auto& original=candidate.uniformPlan.slices[n];
        auto& target=uniformSources[n];
        target.buffer=reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(original.buffer.identity.handle));
        target.size=original.buffer.size;target.usage=original.buffer.usage;
        target.effectiveOffset=original.effectiveOffset;target.bytes=original.effectiveRange;
    }
    frame_uniform_copy::Dispatch uniformDispatch;
    if(!d->uniformCopy.admit(uniformSources.data(),uniformSources.size(),dispatch.totalBytes(),uniformDispatch)) {
        uniformRefuse(*d,UniformRuntimeRefusal::StagingCapacity);return;
    }
#endif
    frame_pixels::Source source;
    source.image = reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(image.identity.handle));
    source.generation=image.identity.generation; source.format=image.format; source.extent=image.extent;
    source.usage=image.usage; source.flags=image.flags; source.type=image.type; source.mipLevels=image.mipLevels;
    source.arrayLayers=image.arrayLayers; source.samples=image.samples; source.layout=candidate.binding.imageLayout;
    frame_pixels::Admission admission;
    admission.commandGeneration=candidate.commandGeneration; admission.complete=true; admission.primary=true;
    admission.outsideRenderPass=true; admission.afterMatchingDistortDraw=true; admission.queueOwnsImage=true;
    admission.beforeExternalRelease=true; admission.ordinaryUnprotected=true;
    admission.oneTimeSubmit=candidate.oneTimeSubmit;
    admission.noSimultaneousUse=candidate.noSimultaneousUse;
    admission.retirementRequired=candidate.retirementRequired;
    if (d->readback.record(command,source,admission) == frame_pixels::Status::Ok) {
        // Only the successful raw-copy CAS owner may update the one prepared set.
        // Both operations stay within this same valid native primary recording.
        const auto result=d->sampler.record(command,dispatch);
        d->candidate=candidate;d->samplerDispatch=dispatch;
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
        d->bridge.copyRecorded(candidate);
#endif
        d->samplerRecorded=result==frame_pixel_sampler::Status::Ok;
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
        d->uniformDispatch=uniformDispatch;
        d->uniformRecorded=d->samplerRecorded &&
            d->uniformCopy.record(command,uniformDispatch)==frame_uniform_copy::Status::Ok;
#endif
        d->selected.store(true,std::memory_order_release);
    }
}
} // namespace

PIXEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL frame_pixel_vkGetInstanceProcAddr(VkInstance,const char*);
PIXEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL frame_pixel_vkGetDeviceProcAddr(VkDevice,const char*);
namespace {
VKAPI_ATTR VkResult VKAPI_CALL traceCreateInstance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* alloc,VkInstance* out) {
    auto* link=static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while(link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO))
        link=static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if(!link || !link->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_ptr<Instance> record;
    try{record=std::make_unique<Instance>();}catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
    record->gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    record->gpdpa=link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    auto next=reinterpret_cast<PFN_vkCreateInstance>(record->gipa(VK_NULL_HANDLE,"vkCreateInstance"));
    link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    const auto result=next(info,alloc,out);
    if(result==VK_SUCCESS){
        record->native=*out;
        record->apiVersion=info->pApplicationInfo && info->pApplicationInfo->apiVersion
            ?info->pApplicationInfo->apiVersion:VK_API_VERSION_1_0;
        const auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(record->gipa(*out,"vkDestroyInstance"));
        try{auto& c=contexts();std::lock_guard<std::mutex> lock(c.mutex);c.instances.emplace(dispatchKey(*out),std::move(record));}
        catch(...){destroy(*out,alloc);*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceDestroyInstance(VkInstance instance,const VkAllocationCallbacks* alloc) {
    auto* i=instanceFor(instance);auto next=reinterpret_cast<PFN_vkDestroyInstance>(i->gipa(instance,"vkDestroyInstance"));
    const auto key=dispatchKey(instance);next(instance,alloc);auto& c=contexts();std::lock_guard<std::mutex> lock(c.mutex);c.instances.erase(key);
}
// A non-null core pointer does not authorize substituting it for an enabled
// extension command (or the reverse). Preserve the exact queried API name.
void loadRenderPass2Dispatch(Device& d,VkDevice device) {
#define LOAD_RP2(name) d.name=reinterpret_cast<PFN_vk##name>(d.gdpa(device,"vk" #name));
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    LOAD_RP2(CreateRenderPass2) LOAD_RP2(CreateRenderPass2KHR)
#endif
    LOAD_RP2(CmdBeginRenderPass2) LOAD_RP2(CmdBeginRenderPass2KHR)
    LOAD_RP2(CmdNextSubpass2) LOAD_RP2(CmdNextSubpass2KHR)
    LOAD_RP2(CmdEndRenderPass2) LOAD_RP2(CmdEndRenderPass2KHR)
#undef LOAD_RP2
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d.nativeSubpassInputs={d.native,reinterpret_cast<PFN_vkVoidFunction>(d.CmdBeginRenderPass2),reinterpret_cast<PFN_vkVoidFunction>(d.CmdBeginRenderPass2KHR),reinterpret_cast<PFN_vkVoidFunction>(d.CmdNextSubpass2),reinterpret_cast<PFN_vkVoidFunction>(d.CmdNextSubpass2KHR)};
#endif
}
void loadRecordingAliasDispatch(Device& d,VkDevice device) {
#define LOAD_RECORDING(name) d.name=reinterpret_cast<PFN_vk##name>(d.gdpa(device,"vk" #name));
    LOAD_RECORDING(CmdDispatchBase) LOAD_RECORDING(CmdDispatchBaseKHR)
    LOAD_RECORDING(CmdBeginRendering) LOAD_RECORDING(CmdBeginRenderingKHR) LOAD_RECORDING(CmdEndRendering) LOAD_RECORDING(CmdEndRenderingKHR)
    LOAD_RECORDING(QueueSubmit2) LOAD_RECORDING(QueueSubmit2KHR)
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    LOAD_RECORDING(CmdDrawIndirectCount) LOAD_RECORDING(CmdDrawIndirectCountKHR) LOAD_RECORDING(CmdDrawIndexedIndirectCount) LOAD_RECORDING(CmdDrawIndexedIndirectCountKHR)
#endif
#undef LOAD_RECORDING
}
VKAPI_ATTR VkResult VKAPI_CALL traceCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* out) {
    auto* i=instanceFor(physical);auto* link=static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while(link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO))
        link=static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    if(!i || !link || !link->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_ptr<Device> d;
    try{d=std::make_unique<Device>();}catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
    d->gdpa=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto next=reinterpret_cast<PFN_vkCreateDevice>(link->u.pLayerInfo->pfnNextGetInstanceProcAddr(i->native,"vkCreateDevice"));
    link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    const auto result=next(physical,info,alloc,out);if(result!=VK_SUCCESS)return result;
    d->native=*out;d->id=++nextDevice;
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(session().allowed){
    VkPhysicalDeviceProperties nativeProperties{};VkPhysicalDeviceMemoryProperties nativeMemory{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(i->gipa(i->native,"vkGetPhysicalDeviceProperties"))(physical,&nativeProperties);
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(i->gipa(i->native,"vkGetPhysicalDeviceMemoryProperties"))(physical,&nativeMemory);
    d->bridge.deviceCreated(*out,*info,d->gdpa,nativeProperties,nativeMemory);
    session().tracker->ssboDeviceCreated(d->id,*out);
    }
#endif
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    // The successful call's pointers are still readable HERE. This observes
    // enabled state without modifying the native feature/extension request.
    observeDefinitions(d.get(),[&](auto& definitions){definitions.createDevice(*out,*info,result);});
#endif
    d->instanceApiVersion=i->apiVersion;
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    {
        // No graphics/compute queue observation, reader, sampler, command
        // tracker or recording writer is prepared in this functional mode.
#define LOAD_PRESENT(name) d->name=reinterpret_cast<PFN_vk##name>(d->gdpa(*out,"vk" #name));
        LOAD_PRESENT(DestroyDevice) LOAD_PRESENT(CreateShaderModule) LOAD_PRESENT(DestroyShaderModule)
        LOAD_PRESENT(CreateGraphicsPipelines) LOAD_PRESENT(DestroyPipeline)
        LOAD_PRESENT(CreateDescriptorSetLayout) LOAD_PRESENT(DestroyDescriptorSetLayout)
        LOAD_PRESENT(CreatePipelineLayout) LOAD_PRESENT(DestroyPipelineLayout)
        LOAD_PRESENT(CreateRenderPass) LOAD_PRESENT(CreateRenderPass2) LOAD_PRESENT(CreateRenderPass2KHR) LOAD_PRESENT(DestroyRenderPass)
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
        LOAD_PRESENT(DeviceWaitIdle) LOAD_PRESENT(GetDeviceQueue) LOAD_PRESENT(GetDeviceQueue2)
        LOAD_PRESENT(AllocateCommandBuffers) LOAD_PRESENT(FreeCommandBuffers) LOAD_PRESENT(BeginCommandBuffer) LOAD_PRESENT(EndCommandBuffer)
        LOAD_PRESENT(ResetCommandBuffer) LOAD_PRESENT(ResetCommandPool) LOAD_PRESENT(DestroyCommandPool)
        LOAD_PRESENT(CmdBindPipeline) LOAD_PRESENT(CmdDraw) LOAD_PRESENT(CmdDrawIndexed)
        LOAD_PRESENT(CmdBeginQuery) LOAD_PRESENT(CmdBeginQueryIndexedEXT) LOAD_PRESENT(CmdExecuteCommands)
        LOAD_PRESENT(CmdBindShadersEXT) LOAD_PRESENT(QueueSubmit) LOAD_PRESENT(QueueSubmit2) LOAD_PRESENT(QueueSubmit2KHR)
        LOAD_PRESENT(CmdBindDescriptorSets) LOAD_PRESENT(CmdPushDescriptorSetKHR) LOAD_PRESENT(CmdPushDescriptorSetWithTemplateKHR)
        LOAD_PRESENT(CmdBindDescriptorBuffersEXT) LOAD_PRESENT(CmdSetDescriptorBufferOffsetsEXT) LOAD_PRESENT(CmdBindDescriptorBufferEmbeddedSamplersEXT)
        LOAD_PRESENT(CmdBindDescriptorSets2KHR) LOAD_PRESENT(CmdPushDescriptorSet2KHR) LOAD_PRESENT(CmdPushDescriptorSetWithTemplate2KHR)
        LOAD_PRESENT(CmdSetDescriptorBufferOffsets2EXT) LOAD_PRESENT(CmdBindDescriptorBufferEmbeddedSamplers2EXT)
        VkPhysicalDeviceMemoryProperties diagnosticMemory{};
        auto memoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(i->gipa(i->native,"vkGetPhysicalDeviceMemoryProperties"));
        if(session().allowed && memoryProperties){
            memoryProperties(physical,&diagnosticMemory);
            auto queueProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(i->gipa(i->native,"vkGetPhysicalDeviceQueueFamilyProperties"));
            const auto family=info->queueCreateInfoCount&&info->pQueueCreateInfos?info->pQueueCreateInfos[0].queueFamilyIndex:UINT32_MAX;
            const auto observedQueue=dashboard_transition_probe::observeQueueFamily(queueProperties,physical,family);
            d->dashboardProbe.prepare(*out,*info,d->gdpa,diagnosticMemory,session().config.directoryFd,observedQueue.flags,&observedQueue);
        }
#endif
#undef LOAD_PRESENT
        auto* published=d.get();const auto destroy=d->DestroyDevice;
        try{auto& c=contexts();std::lock_guard<std::mutex> lock(c.mutex);c.devices.emplace(dispatchKey(*out),std::move(d));}
        catch(...){destroy(*out,alloc);*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
        if(session().allowed){published->presenterRegistered=true;presenterDeviceStarted(*out,published->instanceApiVersion);}
        return result;
    }
#endif
    frame_pixel_sampler::ReviewedRoutes reviewed;
    reviewed.coreComputeRoutesObserved=VK_API_VERSION_VARIANT(i->apiVersion)==0 &&
        VK_API_VERSION_MAJOR(i->apiVersion)==1 && VK_API_VERSION_MINOR(i->apiVersion)<=4;
    reviewed.queryRoutesObserved=reviewed.coreComputeRoutesObserved;
    reviewed.extensionNames=frame_pixel_sampler_routes::DeviceExtensions;
    reviewed.extensionCount=sizeof(frame_pixel_sampler_routes::DeviceExtensions)/sizeof(*frame_pixel_sampler_routes::DeviceExtensions);
    reviewed.additionalChainTypes=frame_pixel_sampler_routes::DeviceChainTypes;
    reviewed.additionalChainTypeCount=sizeof(frame_pixel_sampler_routes::DeviceChainTypes)/sizeof(*frame_pixel_sampler_routes::DeviceChainTypes);
    d->samplerEligibility=frame_pixel_sampler::inspectDeviceCreate(*info,reviewed);
    // Retain only the first unknown, bounded ASCII extension for the cold
    // report; never retain borrowed vkCreateDevice input pointers or long lists.
    if(info->enabledExtensionCount<=128 && info->ppEnabledExtensionNames)
        for(std::uint32_t n=0;n<info->enabledExtensionCount && !d->unreviewedExtension[0];++n) {
            const auto* name=info->ppEnabledExtensionNames[n];bool known=false;
            for(std::uint32_t k=0;name && k<reviewed.extensionCount;++k)
                known=known || frame_pixel_sampler::exactExtensionName(name,reviewed.extensionNames[k]);
            if(!known && name)for(std::size_t k=0;k<sizeof(d->unreviewedExtension)-1 && name[k];++k)
                d->unreviewedExtension[k]=(name[k]>='A' && name[k]<='Z') || (name[k]>='a' && name[k]<='z') ||
                    (name[k]>='0' && name[k]<='9') || name[k]=='_' ?name[k]:'_';
        }
#define LOAD(name) d->name=reinterpret_cast<PFN_vk##name>(d->gdpa(*out,"vk" #name));
    LOAD(DestroyDevice) LOAD(DeviceWaitIdle) LOAD(CreateShaderModule) LOAD(DestroyShaderModule)
    LOAD(CreateGraphicsPipelines) LOAD(DestroyPipeline) LOAD(CreateCommandPool)
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    LOAD(CreateRenderPass) LOAD(DestroyRenderPass)
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    LOAD(GetDeviceQueue) LOAD(GetDeviceQueue2)
#endif
#endif
    LOAD(AllocateCommandBuffers) LOAD(FreeCommandBuffers) LOAD(BeginCommandBuffer) LOAD(EndCommandBuffer)
    LOAD(ResetCommandBuffer) LOAD(ResetCommandPool) LOAD(DestroyCommandPool) LOAD(CmdBindPipeline)
    LOAD(CmdDraw) LOAD(CmdDrawIndexed) LOAD(CmdDrawIndirect) LOAD(CmdDrawIndexedIndirect) LOAD(CmdExecuteCommands)
    LOAD(CmdBindShadersEXT)
    LOAD(CmdDispatch) LOAD(CmdDispatchIndirect)
    LOAD(CmdBeginQuery) LOAD(CmdBeginQueryIndexedEXT)
    LOAD(CmdBeginRenderPass) LOAD(CmdNextSubpass) LOAD(CmdEndRenderPass)
    LOAD(QueueSubmit)
#define BINDING_FIELD(name) LOAD(name)
#include "passive_binding_commands.inc"
#undef BINDING_FIELD
#undef LOAD
    loadRenderPass2Dispatch(*d,*out);loadRecordingAliasDispatch(*d,*out);

#define BINDING_ALIAS(alias,name) if(!d->name)d->name=reinterpret_cast<PFN_vk##name>(d->gdpa(*out,alias));
#include "passive_binding_aliases.inc"
#undef BINDING_ALIAS
    bool ordinary=info->queueCreateInfoCount>0 && info->queueCreateInfoCount<=32;
    for(std::uint32_t n=0;ordinary && n<info->queueCreateInfoCount;++n)ordinary=info->pQueueCreateInfos[n].flags==0;
    auto* deviceExtension=static_cast<const VkBaseInStructure*>(info->pNext);
    for(unsigned budget=0;ordinary && deviceExtension && budget<32;++budget,deviceExtension=deviceExtension->pNext)
        if(deviceExtension->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)ordinary=false;
    if(deviceExtension)ordinary=false;
    // Publish forwarding context BEFORE preparing resources or starting a
    // writer. A map allocation failure must not destroy a joinable std::thread
    // or strand a successfully created native device.
    Device* published=d.get();const auto destroy=d->DestroyDevice;
    try{auto& c=contexts();std::lock_guard<std::mutex> lock(c.mutex);c.devices.emplace(dispatchKey(*out),std::move(d));}
    catch(...){destroy(*out,alloc);*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
    if(session().allowed && ordinary && !session().deviceClaimed.exchange(true)) {
        VkPhysicalDeviceMemoryProperties properties{};
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(i->gipa(i->native,"vkGetPhysicalDeviceMemoryProperties"))(physical,&properties);
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
        const auto deviceProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(i->gipa(i->native,"vkGetPhysicalDeviceProperties"));
        if(deviceProperties) {
            VkPhysicalDeviceProperties nativeProperties{};deviceProperties(physical,&nativeProperties);
            published->uniformLimits.minUniformBufferOffsetAlignment=nativeProperties.limits.minUniformBufferOffsetAlignment;
            published->uniformLimits.maxUniformBufferRange=nativeProperties.limits.maxUniformBufferRange;
        }
#endif
        auto queueProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(i->gipa(i->native,"vkGetPhysicalDeviceQueueFamilyProperties"));
        std::array<VkQueueFamilyProperties,64> queueData{};
        std::uint32_t queueCount=0;
        if(queueProperties)queueProperties(physical,&queueCount,nullptr);
        if(queueCount && queueCount<=queueData.size()) {
            auto count=queueCount;queueProperties(physical,&count,queueData.data());
            if(count==queueCount) {
                published->queueFamilyCount=count;
                for(std::uint32_t n=0;n<count;++n)published->queueFlags[n]=queueData[n].queueFlags;
                for(std::uint32_t n=0;n<info->queueCreateInfoCount;++n) {
                    const auto family=info->pQueueCreateInfos[n].queueFamilyIndex;
                    published->nativeComputeQueue=published->nativeComputeQueue ||
                        (family<count && frame_pixel_sampler::queueSupportsCompute(queueData[family].queueFlags));
                }
            }
        }
        auto formatProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(i->gipa(i->native,"vkGetPhysicalDeviceFormatProperties2"));
        if(!formatProperties)formatProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(i->gipa(i->native,"vkGetPhysicalDeviceFormatProperties2KHR"));
        auto imageProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(i->gipa(i->native,"vkGetPhysicalDeviceImageFormatProperties2"));
        if(!imageProperties)imageProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(i->gipa(i->native,"vkGetPhysicalDeviceImageFormatProperties2KHR"));
        if(published->samplerEligibility.eligible())
            published->samplerFormats=frame_pixel_sampler_format::querySupport(physical,formatProperties,imageProperties);
        frame_pixels::Config readbackConfig;readbackConfig.sampledCompanion=true;
        if(published->readback.prepare(*out,published->gdpa,properties,readbackConfig)==frame_pixels::Status::Ok) {
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
            frame_uniform_copy::Preparation uniformPreparation;
            uniformPreparation.target=published->readback.companionBuffer();
            uniformPreparation.capacity=published->readback.companionCapacity();
            // Readback::prepare with sampledCompanion creates these exact flags.
            uniformPreparation.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            const auto alignment=published->uniformLimits.minUniformBufferOffsetAlignment;
            published->uniformPrepared=alignment && !(alignment&(alignment-1)) &&
                published->uniformLimits.maxUniformBufferRange>=frame_pixel_uniform::CandidateMatrixEnd &&
                published->uniformCopy.prepare(*out,published->gdpa,uniformPreparation)==frame_uniform_copy::Status::Ok;
#endif
            if(published->samplerEligibility.eligible() && published->nativeComputeQueue && published->samplerFormats.complete && published->samplerFormats.routeCount) {
                frame_pixel_sampler::Preparation preparation;
                preparation.targetBuffer=published->readback.companionBuffer();preparation.capacity=published->readback.companionCapacity();
                preparation.samplerYcbcrConversionEnabled=published->samplerEligibility.samplerYcbcrConversionEnabled;
                preparation.combinedImageSamplerDescriptorCount=published->samplerFormats.poolDescriptorCount;
                published->samplerPrepareAttempted=true;
                const auto prepared=published->sampler.prepare(*out,published->gdpa,preparation);
                published->samplerPrepareResult=prepared==frame_pixel_sampler::Status::Ok?VK_SUCCESS:published->sampler.lastResult();
            }
            published->ownsCapture=true;
            try{published->writer=std::thread(writer,published);}
            catch(...){published->ownsCapture=false;published->sampler.release();published->readback.release();}
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceDestroyDevice(VkDevice device,const VkAllocationCallbacks* alloc) {
    auto* d=deviceFor(device);
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    {
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
        d->dashboardProbe.release(d->DeviceWaitIdle ? d->DeviceWaitIdle(device) : VK_NOT_READY);
#endif
        if(d->presenterRegistered)presenterDeviceStopped();
        observeDefinitions(d,[&](auto& definitions){definitions.destroyDevice();});
        const auto key=dispatchKey(device);d->DestroyDevice(device,alloc);
        auto& c=contexts();std::lock_guard<std::mutex> lock(c.mutex);c.devices.erase(key);return;
    }
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    // vkDestroyDevice entry externally closes valid device/queue callbacks.
    // Revoke first so a racing cold verifier cannot republish an old lease.
    d->nativeSubpassLease.revoke();
#endif
    d->stop.store(true,std::memory_order_release);if(d->writer.joinable())d->writer.join();
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    observeDefinitions(d,[&](auto& definitions){definitions.destroyDevice();});
#endif
    // Explicit shutdown only, never a rendering/recording callback. The added
    // completion fence also belongs to this queue, so native fence waits alone
    // cannot be substituted for quiescing our own children before destruction.
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    VkResult ssboIdle=VK_NOT_READY;
#endif
    const auto state=d->readback.state();
    if(state!=frame_pixels::State::Empty && state!=frame_pixels::State::Released) {
        const auto idle=d->DeviceWaitIdle(device);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
        ssboIdle=idle;
#endif
        d->sampler.release(idle==VK_SUCCESS || idle==VK_ERROR_DEVICE_LOST);
        d->readback.release(idle==VK_SUCCESS || idle==VK_ERROR_DEVICE_LOST);
    }
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(session().allowed){
        if(d->bridge.snapshot().ready&&ssboIdle==VK_NOT_READY)ssboIdle=d->DeviceWaitIdle(device);
        d->bridge.release(ssboIdle);session().tracker->ssboDeviceDestroyed(d->id);
    }
#endif
    const auto key=dispatchKey(device);d->DestroyDevice(device,alloc);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    // Keep the provider mapping pinned through the final original device call.
    // DeviceDestroy synchronization plus the joined cold worker closes callback
    // entry; release still checks that every scoped permit has returned.
    d->nativeSubpassLease.releaseCold();
#endif
    auto& c=contexts();std::lock_guard<std::mutex> lock(c.mutex);c.devices.erase(key);
}
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
using RecordingOperation=frame_pixel_ssbo_receiver::Operation;
using CommandContext=frame_pixel_ssbo_receiver::CommandContext;
namespace recording=frame_pixel_renderpass_recording;
CommandContext commandContext(RecordingOperation operation,std::uint64_t object=0,std::uint32_t flags=0) noexcept {
    CommandContext out;out.operation=operation;out.source=frame_pixel_ssbo_receiver::ContextSource::Wrapper;
    out.object=object;out.flags=flags;return out;
}
CommandContext passContext(RecordingOperation operation,const recording::Shape& shape) noexcept {
    auto out=commandContext(operation,frame_pixel_ssbo_clone::handle(shape.renderPass));
    out.source=frame_pixel_ssbo_receiver::ContextSource::RenderPassDecoder;
    out.detailReason=unsigned(shape.refusal.reason);out.structureType=shape.refusal.type;
    out.privateProfile=unsigned(shape.privateProfile);out.privatePayloadObserved=shape.privatePayloadObserved;out.privateSrgbMask=shape.privateSrgbMask;
    out.index=shape.refusal.index;out.chain=unsigned(shape.refusal.chain);out.contents=unsigned(shape.contents);
    static_assert(recording::MaxChain==frame_pixel_ssbo_transaction::CommandContextHeaders,"lossless bounded public header census");
    static_assert(unsigned(recording::Chain::Count)==frame_pixel_ssbo_transaction::CommandContextChains,"all public Begin/Next/End roots retained");
    for(unsigned i=0;i<out.chains.size();++i){const auto& source=shape.chains[i];auto& target=out.chains[i];
        target.rootType=source.rootType;target.types=source.types;target.count=source.count;target.termination=unsigned(source.termination);}
    return out;
}
void recordPassContext(Device* d,VkCommandBuffer command,RecordingOperation operation,const recording::Shape& shape) noexcept {
    const auto context=passContext(operation,shape);
    if(shape.supported)d->bridge.observe(command,context);else d->bridge.unknown(command,context);
}
void unknownCommand(Device* d,VkCommandBuffer command,RecordingOperation operation,std::uint64_t object=0,std::uint32_t flags=0) noexcept {
    d->bridge.unknown(command,commandContext(operation,object,flags));
}
#endif
#define observeBindings(...) observeBindingsAt(__func__, __VA_ARGS__)
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
#define FRAME_PIXEL_COMPUTE_BIND(d,command,point) do { const auto context=commandContext(RecordingOperation::CmdBindDescriptorSets,frame_pixel_ssbo_clone::handle(layout),unsigned(point)); if((point)!=VK_PIPELINE_BIND_POINT_GRAPHICS) { session().tracker->touchComputeState((d)->id,command); (d)->bridge.unknown(command,context); } else (d)->bridge.observe(command,context); } while(false)
#else
#define FRAME_PIXEL_COMPUTE_BIND(d,command,point) do { if((point)!=VK_PIPELINE_BIND_POINT_GRAPHICS) session().tracker->touchComputeState((d)->id,command); } while(false)
#endif
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
#define FRAME_PIXEL_DEFINITION_SET_CREATED(d,object,info) observeDefinitions(d,[&](auto& definitions){definitions.createSet(object,info,VK_SUCCESS);})
#define FRAME_PIXEL_DEFINITION_SET_DESTROYED(d,object) observeDefinitions(d,[&](auto& definitions){definitions.destroySet(object);})
#define FRAME_PIXEL_DEFINITION_LAYOUT_CREATED(d,object,info) observeDefinitions(d,[&](auto& definitions){definitions.createLayout(object,info,VK_SUCCESS);})
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
#define FRAME_PIXEL_DEFINITION_LAYOUT_DESTROYED(d,object) do { (d)->bridge.definitionRetiring(frame_pixel_ssbo_journal::ObjectKind::Layout,frame_pixel_ssbo_clone::handle(object)); observeDefinitions(d,[&](auto& definitions){definitions.destroyLayout(object);}); } while(false)
#else
#define FRAME_PIXEL_DEFINITION_LAYOUT_DESTROYED(d,object) observeDefinitions(d,[&](auto& definitions){definitions.destroyLayout(object);})
#endif
#endif
#include "passive_binding_hooks.inc"
#undef FRAME_PIXEL_DEFINITION_SET_CREATED
#undef FRAME_PIXEL_DEFINITION_SET_DESTROYED
#undef FRAME_PIXEL_DEFINITION_LAYOUT_CREATED
#undef FRAME_PIXEL_DEFINITION_LAYOUT_DESTROYED
#undef FRAME_PIXEL_COMPUTE_BIND
#undef observeBindings
VKAPI_ATTR VkResult VKAPI_CALL traceCreateShaderModule(VkDevice device,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* alloc,VkShaderModule* out) {
    auto* d=deviceFor(device);const auto result=d->CreateShaderModule(device,info,alloc,out);
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
    if(result==VK_SUCCESS)d->dashboardProbe.shaderObserved(*out,*info);
#endif
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    if(result==VK_SUCCESS)observeDefinitions(d,[&](auto& definitions){
        if(!definitions.createShader(*out,*info,result)||!session().presenterSource.ready()||
           info->codeSize!=6620||d->presenterInterfaceAuthorized.load(std::memory_order_acquire))return;
        // This cold CPU-only transformer proves the original B9 constant0
        // accesses for pending count1 definitions. Its SSBO artifact is never
        // created as a Vulkan object, exposed, bound or dispatched here.
        auto original=std::make_unique<frame_pixel_ssbo_clone::ShaderDefinition>();
        auto proof=std::make_unique<frame_pixel_ssbo_profile::Artifact>();
        frame_pixel_ssbo_clone::Refusal why;
        if(frame_pixel_ssbo_clone::captureShader(*info,frame_pixel_ssbo_clone::ShaderRole::Fragment,*original,why)&&
           frame_pixel_ssbo_profile::transform(*original,false,*proof,why)&&definitions.authorizeProbeInterface(proof->profile))
            d->presenterInterfaceAuthorized.store(true,std::memory_order_release);
    });
    return result;
#endif
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(result==VK_SUCCESS)d->bridge.shaderCreated(*info,d->definitions);
#endif
    if(result==VK_SUCCESS)observeDefinitions(d,[&](auto& definitions){definitions.createShader(*out,*info,result);});
#endif
    if(result==VK_SUCCESS)session().tracker->createShader(d->id,*out,*info);
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceDestroyShaderModule(VkDevice device,VkShaderModule value,const VkAllocationCallbacks* alloc) {
    auto* d=deviceFor(device);
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    observeDefinitions(d,[&](auto& definitions){definitions.destroyShader(value);});
#endif
#ifndef FRAME_PIXEL_SYNTHETIC_PRESENTER
    session().tracker->destroyShader(d->id,value);
#endif
    d->DestroyShaderModule(device,value,alloc);
}
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
void retirePresenterOriginal(void* owner,VkDevice device,VkPipeline pipeline) noexcept {
    auto* d=static_cast<Device*>(owner);
    if(!d||d->native!=device)return;
    // Retire at destroy ENTRY. The downstream call may allow immediate raw
    // handle reuse on another thread; never retire that later generation.
    // This mode allocates no LiveTracker and registers no diagnostic pipeline.
    // A different cold callback can invalidate observation while the optional
    // driver creation is in flight. Sticky refusal must not skip retirement
    // of the still-unexposed original before downstream handle reuse.
    try{if(!d->definitions.destroyPipeline(pipeline))d->definitionBoundary.invalidate();}
    catch(...){d->definitionBoundary.invalidate();}
    if(d->definitionBoundary.failed())session().presenterCounters.historyFailed();
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL traceCreateGraphicsPipelines(VkDevice device,VkPipelineCache cache,std::uint32_t count,const VkGraphicsPipelineCreateInfo* info,const VkAllocationCallbacks* alloc,VkPipeline* out) {
    auto* d=deviceFor(device);const auto result=d->CreateGraphicsPipelines(device,cache,count,info,alloc,out);
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    if(result==VK_SUCCESS&&count<=256&&info&&out)for(std::uint32_t n=0;n<count;++n){
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
        unsigned dashboardNativeReason=UINT32_MAX;
#endif
        observeDefinitions(d,[&](auto& definitions){
            frame_pixel_ssbo_journal::Event event;
            if(!definitions.createPipeline(out[n],info[n],result,&event)||!session().presenterSource.ready()||
               session().presenterWriterFailed.load(std::memory_order_acquire))return;
            auto ready=std::make_unique<frame_pixel_ssbo_journal::Ready>();
            if(!definitions.lookupPipeline({frame_pixel_ssbo_clone::handle(device),event.value,event.generation},*ready))return;
            const frame_native_presenter::Dispatch dispatch{d->CreateShaderModule,d->DestroyShaderModule,
                                                            d->CreateGraphicsPipelines,d->DestroyPipeline};
            const auto completed=presenter::complete(device,cache,d->instanceApiVersion,result,info[n],alloc,out[n],
                ready->recipe,session().presenterSource,dispatch,{d,retirePresenterOriginal});
            session().presenterCounters.attempted(completed);
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
            dashboardNativeReason=unsigned(completed.reason);
            if(completed.replaced)d->dashboardProbe.pipeline(out[n],cache,info[n],ready->recipe);
#endif
            // Success returns a normal app-owned candidate pipeline. It is
            // deliberately NOT registered as a stock-original-PS pipeline.
        });
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
        d->dashboardProbe.pipelineObserved(out[n],info[n],dashboardNativeReason);
#endif
    }
    return result;
#endif
    // Partial success handles are not an admitted diagnostic pipeline.
    if(result==VK_SUCCESS && count<=256)for(std::uint32_t n=0;n<count;++n){
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
        frame_pixel_ssbo_journal::Event definitionEvent;
        observeDefinitions(d,[&](auto& definitions){definitions.createPipeline(out[n],info[n],result,&definitionEvent);});
#endif
        session().tracker->createPipeline(d->id,out[n],info[n]);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
        if(definitionEvent.reason==frame_pixel_ssbo_journal::Reason::None&&definitionEvent.generation)
            session().tracker->ssboPipelineCreated(d->id,out[n],{frame_pixel_ssbo_clone::handle(device),definitionEvent.value,definitionEvent.generation});
#endif
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceDestroyPipeline(VkDevice device,VkPipeline pipeline,const VkAllocationCallbacks* alloc) {
    auto* d=deviceFor(device);
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
    d->dashboardProbe.pipelineRetiring(pipeline);
#endif
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.definitionRetiring(frame_pixel_ssbo_journal::ObjectKind::Pipeline,frame_pixel_ssbo_clone::handle(pipeline));
#endif
    observeDefinitions(d,[&](auto& definitions){definitions.destroyPipeline(pipeline);});
#endif
#ifndef FRAME_PIXEL_SYNTHETIC_PRESENTER
    session().tracker->destroyPipeline(d->id,pipeline);
#endif
    d->DestroyPipeline(device,pipeline,alloc);
}
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
VKAPI_ATTR VkResult VKAPI_CALL traceCreateRenderPass(VkDevice device,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* alloc,VkRenderPass* out) {
    auto* d=deviceFor(device);const auto result=d->CreateRenderPass(device,info,alloc,out);
    if(result==VK_SUCCESS)observeDefinitions(d,[&](auto& definitions){definitions.createRenderPass(*out,*info,result);});
    return result;
}
VkResult createRenderPass2(Device* d,PFN_vkCreateRenderPass2 original,bool khr,VkDevice device,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* alloc,VkRenderPass* out) {
    const auto result=original(device,info,alloc,out);
    if(result==VK_SUCCESS)observeDefinitions(d,[&](auto& definitions){definitions.createRenderPass2(*out,*info,result,nullptr,khr);});
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL traceCreateRenderPass2(VkDevice device,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* alloc,VkRenderPass* out) {
    auto* d=deviceFor(device);return createRenderPass2(d,d->CreateRenderPass2,false,device,info,alloc,out);
}
VKAPI_ATTR VkResult VKAPI_CALL traceCreateRenderPass2KHR(VkDevice device,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* alloc,VkRenderPass* out) {
    auto* d=deviceFor(device);return createRenderPass2(d,d->CreateRenderPass2KHR,true,device,info,alloc,out);
}
VKAPI_ATTR void VKAPI_CALL traceDestroyRenderPass(VkDevice device,VkRenderPass value,const VkAllocationCallbacks* alloc) {
    auto* d=deviceFor(device);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.definitionRetiring(frame_pixel_ssbo_journal::ObjectKind::RenderPass,frame_pixel_ssbo_clone::handle(value));
#endif
    observeDefinitions(d,[&](auto& definitions){definitions.destroyRenderPass(value);});d->DestroyRenderPass(device,value,alloc);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL traceCreateCommandPool(VkDevice device,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* alloc,VkCommandPool* out) {
    auto* d=deviceFor(device);const auto result=d->CreateCommandPool(device,info,alloc,out);
    if(result==VK_SUCCESS)session().tracker->createCommandPool(d->id,*out,*info);
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceDestroyCommandPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* alloc) {
    auto* d=deviceFor(device);
    const auto before=d->selected.load(std::memory_order_acquire)
        ? retiring(d,d->candidate.command,pool) : RetirementTicket{};
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto ssbo=d->bridge.beforeRetirement(frame_pixel_ssbo_transaction::Retirement::DestroyPool,VK_NULL_HANDLE,pool);
#endif
    d->DestroyCommandPool(device,pool,alloc);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.afterRetirement(ssbo,VK_SUCCESS);
#endif
    retired(d,before,true);
    session().tracker->destroyCommandPool(d->id,pool);
}
VKAPI_ATTR VkResult VKAPI_CALL traceResetCommandPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    auto* d=deviceFor(device);
    const auto before=d->selected.load(std::memory_order_acquire)
        ? retiring(d,d->candidate.command,pool) : RetirementTicket{};
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto ssbo=d->bridge.beforeRetirement(frame_pixel_ssbo_transaction::Retirement::ResetPool,VK_NULL_HANDLE,pool);
#endif
    const auto result=d->ResetCommandPool(device,pool,flags);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.afterRetirement(ssbo,result);
#endif
    retired(d,before,result==VK_SUCCESS);
    if(result==VK_SUCCESS)session().tracker->resetCommandPool(d->id,pool);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL traceAllocateCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    auto* d=deviceFor(device);const auto result=d->AllocateCommandBuffers(device,info,out);
    if(result==VK_SUCCESS)session().tracker->allocateCommands(d->id,*info,out);
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceFreeCommandBuffers(VkDevice device,VkCommandPool pool,std::uint32_t count,const VkCommandBuffer* commands) {
    auto* d=deviceFor(device);
    RetirementTicket before;
    if(d->selected.load(std::memory_order_acquire) && count<=512)
        for(std::uint32_t n=0;n<count;++n)
            if(commands[n]==d->candidate.command)before=retiring(d,commands[n],pool);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    frame_pixel_ssbo_receiver::Bridge::RetirementTicket ssbo;
    if(count<=512)for(std::uint32_t n=0;n<count;++n){d->bridge.forget(commands[n],pool);if(d->bridge.tracks(commands[n]))
        ssbo=d->bridge.beforeRetirement(frame_pixel_ssbo_transaction::Retirement::FreeCommand,commands[n],pool);}
#endif
    d->FreeCommandBuffers(device,pool,count,commands);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.afterRetirement(ssbo,VK_SUCCESS);
#endif
    retired(d,before,true);
    session().tracker->freeCommands(d->id,pool,count,commands);
}
VKAPI_ATTR VkResult VKAPI_CALL traceBeginCommandBuffer(VkCommandBuffer command,const VkCommandBufferBeginInfo* info) {
    auto* d=deviceFor(command);
    const auto before=retiring(d,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto ssbo=d->bridge.beforeRetirement(frame_pixel_ssbo_transaction::Retirement::BeginCommand,command);
#endif
    const auto result=d->BeginCommandBuffer(command,info);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.afterRetirement(ssbo,result);
#endif
    retired(d,before,result==VK_SUCCESS);
    session().tracker->beginCommand(d->id,command,*info,result==VK_SUCCESS);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(result==VK_SUCCESS){d->bridge.begin(*session().tracker,d->id,command,session().armed.load(std::memory_order_acquire));
        auto context=commandContext(RecordingOperation::BeginCommandBuffer,0,info->flags);context.chains[0].rootType=unsigned(info->sType);d->bridge.observe(command,context);}
#endif
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL traceEndCommandBuffer(VkCommandBuffer command) {
    auto* d=deviceFor(command);const auto result=d->EndCommandBuffer(command);
    session().tracker->endCommand(d->id,command,result==VK_SUCCESS);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.observe(command,commandContext(RecordingOperation::EndCommandBuffer));d->bridge.endCommand(command,result);
#endif
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL traceResetCommandBuffer(VkCommandBuffer command,VkCommandBufferResetFlags flags) {
    auto* d=deviceFor(command);
    const auto before=retiring(d,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto ssbo=d->bridge.beforeRetirement(frame_pixel_ssbo_transaction::Retirement::ResetCommand,command);
#endif
    const auto result=d->ResetCommandBuffer(command,flags);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.afterRetirement(ssbo,result);
#endif
    retired(d,before,result==VK_SUCCESS);
    if(result==VK_SUCCESS)session().tracker->resetCommand(d->id,command);
    return result;
}
VKAPI_ATTR void VKAPI_CALL traceCmdBindPipeline(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline) {
    auto* d=deviceFor(command);session().tracker->bindPipeline(d->id,command,point,pipeline);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){d->bridge.observe(command,commandContext(RecordingOperation::CmdBindPipeline,frame_pixel_ssbo_clone::handle(pipeline),unsigned(point)));d->bridge.pipeline(command,pipeline);}
    else unknownCommand(d,command,RecordingOperation::CmdBindPipeline,frame_pixel_ssbo_clone::handle(pipeline),unsigned(point));
#endif
    d->CmdBindPipeline(command,point,pipeline);
}
VKAPI_ATTR void VKAPI_CALL traceCmdBindShadersEXT(VkCommandBuffer command,std::uint32_t count,const VkShaderStageFlagBits* stages,const VkShaderEXT* shaders) {
    auto* d=deviceFor(command);session().tracker->unknownCommand(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdBindShadersEXT,0,count);
#endif
    d->CmdBindShadersEXT(command,count,stages,shaders);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDispatch(VkCommandBuffer command,std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    auto* d=deviceFor(command);session().tracker->touchComputeState(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdDispatch);
#endif
    d->CmdDispatch(command,x,y,z);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDispatchIndirect(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset) {
    auto* d=deviceFor(command);session().tracker->touchComputeState(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdDispatchIndirect,frame_pixel_ssbo_clone::handle(buffer));
#endif
    d->CmdDispatchIndirect(command,buffer,offset);
}
void dispatchBase(Device* d,PFN_vkCmdDispatchBase original,bool khr,VkCommandBuffer command,std::uint32_t bx,std::uint32_t by,std::uint32_t bz,std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    session().tracker->touchComputeState(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,khr?RecordingOperation::CmdDispatchBaseKHR:RecordingOperation::CmdDispatchBase);
#endif
    (void)khr;original(command,bx,by,bz,x,y,z);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDispatchBase(VkCommandBuffer command,std::uint32_t bx,std::uint32_t by,std::uint32_t bz,std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    auto* d=deviceFor(command);dispatchBase(d,d->CmdDispatchBase,false,command,bx,by,bz,x,y,z);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDispatchBaseKHR(VkCommandBuffer command,std::uint32_t bx,std::uint32_t by,std::uint32_t bz,std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    auto* d=deviceFor(command);dispatchBase(d,d->CmdDispatchBaseKHR,true,command,bx,by,bz,x,y,z);
}
// Queries must begin/end in their own command buffer; accepted primaries cannot
// inherit them and never execute secondary recordings. Refuse every recording
// which has begun ANY query, even if it has already ended. Sharing the existing
// monotonic sampler-history bit preserves generation/reset/end-scope semantics.
// Merely enabling pipelineStatisticsQuery on a device is not an active query.
VKAPI_ATTR void VKAPI_CALL traceCmdBeginQuery(VkCommandBuffer command,VkQueryPool pool,std::uint32_t query,VkQueryControlFlags flags) {
    auto* d=deviceFor(command);
    if(session().tracker->touchComputeState(d->id,command))d->queryRoutesUsed.fetch_or(1u,std::memory_order_relaxed);
    #ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdBeginQuery,frame_pixel_ssbo_clone::handle(pool),flags);
#endif
    d->CmdBeginQuery(command,pool,query,flags);
}
VKAPI_ATTR void VKAPI_CALL traceCmdBeginQueryIndexedEXT(VkCommandBuffer command,VkQueryPool pool,std::uint32_t query,VkQueryControlFlags flags,std::uint32_t index) {
    auto* d=deviceFor(command);
    if(session().tracker->touchComputeState(d->id,command))d->queryRoutesUsed.fetch_or(2u,std::memory_order_relaxed);
    #ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdBeginQueryIndexedEXT,frame_pixel_ssbo_clone::handle(pool),flags);
#endif
    d->CmdBeginQueryIndexedEXT(command,pool,query,flags,index);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDraw(VkCommandBuffer command,std::uint32_t vertices,std::uint32_t instances,std::uint32_t firstVertex,std::uint32_t firstInstance) {
    auto* d=deviceFor(command);
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    const auto* limits=session().armed.load(std::memory_order_acquire) && !d->selected.load(std::memory_order_acquire)
        ?&d->uniformLimits:nullptr;
    session().tracker->draw(d->id,command,limits,vertices && instances);
#elif defined(FRAME_PIXEL_SSBO_TRANSACTION)
    session().tracker->draw(d->id,command,nullptr,vertices && instances);
#else
    if(vertices && instances)session().tracker->draw(d->id,command);
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.observe(command,commandContext(RecordingOperation::CmdDraw));
    if(d->bridge.draw(*session().tracker,d->id,command,vertices,instances,firstVertex,firstInstance,0,false))return;
#endif
    d->CmdDraw(command,vertices,instances,firstVertex,firstInstance);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndexed(VkCommandBuffer command,std::uint32_t indices,std::uint32_t instances,std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance) {
    auto* d=deviceFor(command);
#ifdef FRAME_PIXEL_UNIFORM_COMPANION
    const auto* limits=session().armed.load(std::memory_order_acquire) && !d->selected.load(std::memory_order_acquire)
        ?&d->uniformLimits:nullptr;
    session().tracker->draw(d->id,command,limits,indices && instances);
#elif defined(FRAME_PIXEL_SSBO_TRANSACTION)
    session().tracker->draw(d->id,command,nullptr,indices && instances);
#else
    if(indices && instances)session().tracker->draw(d->id,command);
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.observe(command,commandContext(RecordingOperation::CmdDrawIndexed));
    if(d->bridge.draw(*session().tracker,d->id,command,indices,instances,firstIndex,firstInstance,vertexOffset,true))return;
#endif
    d->CmdDrawIndexed(command,indices,instances,firstIndex,vertexOffset,firstInstance);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndirect(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,std::uint32_t count,std::uint32_t stride) {
    auto* d=deviceFor(command);session().tracker->unknownCommand(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdDrawIndirect,frame_pixel_ssbo_clone::handle(buffer),count);
#endif
    d->CmdDrawIndirect(command,buffer,offset,count,stride);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndexedIndirect(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,std::uint32_t count,std::uint32_t stride) {
    auto* d=deviceFor(command);session().tracker->unknownCommand(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdDrawIndexedIndirect,frame_pixel_ssbo_clone::handle(buffer),count);
#endif
    d->CmdDrawIndexedIndirect(command,buffer,offset,count,stride);
}
VKAPI_ATTR void VKAPI_CALL traceCmdExecuteCommands(VkCommandBuffer command,std::uint32_t count,const VkCommandBuffer* children) {
    auto* d=deviceFor(command);session().tracker->unknownCommand(d->id,command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,RecordingOperation::CmdExecuteCommands,0,count);
#endif
    d->CmdExecuteCommands(command,count,children);
}
VKAPI_ATTR void VKAPI_CALL traceCmdBeginRenderPass(VkCommandBuffer command,const VkRenderPassBeginInfo* info,VkSubpassContents contents) {
    auto* d=deviceFor(command);session().tracker->beginRenderPass(d->id,command,contents);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    recording::Shape shape;recording::begin(info,contents,shape);recordPassContext(d,command,RecordingOperation::CmdBeginRenderPass,shape);
    if(shape.supported)d->bridge.beginPass(command,info->renderPass,contents);
#endif
    d->CmdBeginRenderPass(command,info,contents);
}
VKAPI_ATTR void VKAPI_CALL traceCmdNextSubpass(VkCommandBuffer command,VkSubpassContents contents) {
    auto* d=deviceFor(command);session().tracker->nextSubpass(d->id,command,contents);
    d->CmdNextSubpass(command,contents);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    recording::Shape shape;recording::next(contents,shape);recordPassContext(d,command,RecordingOperation::CmdNextSubpass,shape);
    if(shape.supported)d->bridge.nextPass(command,contents);
#endif
}
VKAPI_ATTR void VKAPI_CALL traceCmdEndRenderPass(VkCommandBuffer command) {
    auto* d=deviceFor(command);d->CmdEndRenderPass(command);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    recording::Shape shape;recording::end(shape);recordPassContext(d,command,RecordingOperation::CmdEndRenderPass,shape);d->bridge.endPass(command);
#endif
    afterRenderPass(d,command,false);
}
void beginRenderPass2(Device* d,PFN_vkCmdBeginRenderPass2 original,bool khr,VkCommandBuffer command,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* begin) {
    session().tracker->beginRenderPass(d->id,command,begin->contents);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto operation=khr?RecordingOperation::CmdBeginRenderPass2KHR:RecordingOperation::CmdBeginRenderPass2;
    auto permit=d->nativeSubpassLease.acquire(d->native,khr?frame_pixel_native_driver::Route::Begin2KHR:frame_pixel_native_driver::Route::Begin2,reinterpret_cast<PFN_vkVoidFunction>(original));
    recording::Shape shape;recording::begin2(info,begin,shape,&permit);recordPassContext(d,command,operation,shape);
    if(shape.supported)d->bridge.beginPass(command,info->renderPass,begin->contents);
#endif
    (void)khr;original(command,info,begin);
}
VKAPI_ATTR void VKAPI_CALL traceCmdBeginRenderPass2(VkCommandBuffer command,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* begin) {
    auto* d=deviceFor(command);beginRenderPass2(d,d->CmdBeginRenderPass2,false,command,info,begin);
}
VKAPI_ATTR void VKAPI_CALL traceCmdBeginRenderPass2KHR(VkCommandBuffer command,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* begin) {
    auto* d=deviceFor(command);beginRenderPass2(d,d->CmdBeginRenderPass2KHR,true,command,info,begin);
}
void nextSubpass2(Device* d,PFN_vkCmdNextSubpass2 original,bool khr,VkCommandBuffer command,const VkSubpassBeginInfo* begin,const VkSubpassEndInfo* end) {
    session().tracker->nextSubpass(d->id,command,begin->contents);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    auto permit=d->nativeSubpassLease.acquire(d->native,khr?frame_pixel_native_driver::Route::Next2KHR:frame_pixel_native_driver::Route::Next2,reinterpret_cast<PFN_vkVoidFunction>(original));
#endif
    (void)khr;original(command,begin,end);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto operation=khr?RecordingOperation::CmdNextSubpass2KHR:RecordingOperation::CmdNextSubpass2;
    recording::Shape shape;recording::next2(begin,end,shape,&permit);recordPassContext(d,command,operation,shape);
    if(shape.supported)d->bridge.nextPass(command,begin->contents);
#endif
}
VKAPI_ATTR void VKAPI_CALL traceCmdNextSubpass2(VkCommandBuffer command,const VkSubpassBeginInfo* begin,const VkSubpassEndInfo* end) {
    auto* d=deviceFor(command);nextSubpass2(d,d->CmdNextSubpass2,false,command,begin,end);
}
VKAPI_ATTR void VKAPI_CALL traceCmdNextSubpass2KHR(VkCommandBuffer command,const VkSubpassBeginInfo* begin,const VkSubpassEndInfo* end) {
    auto* d=deviceFor(command);nextSubpass2(d,d->CmdNextSubpass2KHR,true,command,begin,end);
}
void endRenderPass2(Device* d,PFN_vkCmdEndRenderPass2 original,bool khr,VkCommandBuffer command,const VkSubpassEndInfo* end) {
    (void)khr;original(command,end);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto operation=khr?RecordingOperation::CmdEndRenderPass2KHR:RecordingOperation::CmdEndRenderPass2;
    recording::Shape shape;recording::end2(end,shape);recordPassContext(d,command,operation,shape);
    if(shape.supported)d->bridge.endPass(command);
#endif
    afterRenderPass(d,command,false);
}
VKAPI_ATTR void VKAPI_CALL traceCmdEndRenderPass2(VkCommandBuffer command,const VkSubpassEndInfo* end) {
    auto* d=deviceFor(command);endRenderPass2(d,d->CmdEndRenderPass2,false,command,end);
}
VKAPI_ATTR void VKAPI_CALL traceCmdEndRenderPass2KHR(VkCommandBuffer command,const VkSubpassEndInfo* end) {
    auto* d=deviceFor(command);endRenderPass2(d,d->CmdEndRenderPass2KHR,true,command,end);
}
void beginRendering(Device* d,PFN_vkCmdBeginRendering original,bool khr,VkCommandBuffer command,const VkRenderingInfo* info) {
    session().tracker->beginRendering(d->id,command,*info);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    recording::Shape shape;recording::headerCensus(unsigned(info->sType),info->pNext,recording::Chain::RenderPassBegin,shape);
    auto context=passContext(khr?RecordingOperation::CmdBeginRenderingKHR:RecordingOperation::CmdBeginRendering,shape);context.source=frame_pixel_ssbo_receiver::ContextSource::Wrapper;context.flags=info->flags;
    d->bridge.unknown(command,context);
#endif
    (void)khr;original(command,info);
}
VKAPI_ATTR void VKAPI_CALL traceCmdBeginRendering(VkCommandBuffer command,const VkRenderingInfo* info) {auto* d=deviceFor(command);beginRendering(d,d->CmdBeginRendering,false,command,info);}
VKAPI_ATTR void VKAPI_CALL traceCmdBeginRenderingKHR(VkCommandBuffer command,const VkRenderingInfo* info) {auto* d=deviceFor(command);beginRendering(d,d->CmdBeginRenderingKHR,true,command,info);}
void endRendering(Device* d,PFN_vkCmdEndRendering original,bool khr,VkCommandBuffer command) {
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    unknownCommand(d,command,khr?RecordingOperation::CmdEndRenderingKHR:RecordingOperation::CmdEndRendering);
#endif
    (void)khr;original(command);afterRenderPass(d,command,true);
}
VKAPI_ATTR void VKAPI_CALL traceCmdEndRendering(VkCommandBuffer command) {auto* d=deviceFor(command);endRendering(d,d->CmdEndRendering,false,command);}
VKAPI_ATTR void VKAPI_CALL traceCmdEndRenderingKHR(VkCommandBuffer command) {auto* d=deviceFor(command);endRendering(d,d->CmdEndRenderingKHR,true,command);}
template<class Info,class Count,class Command> SubmitTicket ticket(Device* d,std::uint32_t count,const Info* info,Count commandCount,Command command) {
    if(!d->ownsCapture || !d->selected.load(std::memory_order_acquire) || d->readback.state()!=frame_pixels::State::Recorded || count>32)return {};
    for(std::uint32_t i=0;i<count;++i) {
        if(commandCount(info[i])>64)return {};
        for(std::uint32_t j=0;j<commandCount(info[i]);++j)
            if(command(info[i],j)==d->candidate.command) {
                const auto before=session().tracker->prepareSubmit(d->id,d->candidate.command);
                if(frame_pixel_selection::matchesSubmit(d->candidate,before))return before;
                // Consume/refuse a ticket belonging to a later native recording;
                // its work cannot establish completion of the discarded copy.
                session().tracker->acceptSubmit(before,false);
                return {};
            }
    }
    return {};
}
void submitted(Device* d,VkQueue queue,const SubmitTicket& before,VkResult nativeResult) {
    if(!frame_pixel_selection::matchesSubmit(d->candidate,before))return;
    const bool accepted=session().tracker->acceptSubmit(before,nativeResult==VK_SUCCESS);
    if(accepted || nativeResult!=VK_SUCCESS)
        d->readback.submitCompletion(queue,before.command,before.commandGeneration,nativeResult);
}
VKAPI_ATTR VkResult VKAPI_CALL traceQueueSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* info,VkFence fence) {
    auto* d=deviceFor(queue);const auto before=ticket(d,count,info,[](const auto& i){return i.commandBufferCount;},[](const auto& i,std::uint32_t n){return i.pCommandBuffers[n];});
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    const auto ssbo=d->bridge.beforeSubmit(queue,count,info);
#endif
    const auto result=d->QueueSubmit(queue,count,info,fence);submitted(d,queue,before,result);
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.afterSubmit(ssbo,queue,count,info,result);
#endif
    return result;
}
VkResult queueSubmit2(Device* d,PFN_vkQueueSubmit2 original,bool khr,VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    const auto before=ticket(d,count,info,[](const auto& i){return i.commandBufferInfoCount;},[](const auto& i,std::uint32_t n){return i.pCommandBufferInfos[n].commandBuffer;});
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    d->bridge.unsupportedSubmit(queue,count,info,commandContext(khr?RecordingOperation::QueueSubmit2KHR:RecordingOperation::QueueSubmit2));
#endif
    (void)khr;const auto result=original(queue,count,info,fence);submitted(d,queue,before,result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL traceQueueSubmit2(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {auto* d=deviceFor(queue);return queueSubmit2(d,d->QueueSubmit2,false,queue,count,info,fence);}
VKAPI_ATTR VkResult VKAPI_CALL traceQueueSubmit2KHR(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {auto* d=deviceFor(queue);return queueSubmit2(d,d->QueueSubmit2KHR,true,queue,count,info,fence);}
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
VKAPI_ATTR void VKAPI_CALL traceGetDeviceQueue(VkDevice device,std::uint32_t family,std::uint32_t index,VkQueue* out){
    auto* d=deviceFor(device);d->GetDeviceQueue(device,family,index,out);d->bridge.queueRetrieved(family,index,*out);
}
VKAPI_ATTR void VKAPI_CALL traceGetDeviceQueue2(VkDevice device,const VkDeviceQueueInfo2* info,VkQueue* out){
    auto* d=deviceFor(device);d->GetDeviceQueue2(device,info,out);
    d->bridge.queueRetrieved(info->queueFamilyIndex,info->queueIndex,*out,info->sType==VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2&&!info->pNext&&!info->flags);
}
void sharedCmdDrawIndirectCount(Device* d,PFN_vkCmdDrawIndirectCount original,bool khr,VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,std::uint32_t count,std::uint32_t stride){
    session().tracker->unknownCommand(d->id,c);unknownCommand(d,c,khr?RecordingOperation::CmdDrawIndirectCountKHR:RecordingOperation::CmdDrawIndirectCount);original(c,buffer,offset,counts,countOffset,count,stride);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndirectCount(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,std::uint32_t count,std::uint32_t stride){auto* d=deviceFor(c);sharedCmdDrawIndirectCount(d,d->CmdDrawIndirectCount,false,c,buffer,offset,counts,countOffset,count,stride);}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndirectCountKHR(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,std::uint32_t count,std::uint32_t stride){auto* d=deviceFor(c);sharedCmdDrawIndirectCount(d,d->CmdDrawIndirectCountKHR,true,c,buffer,offset,counts,countOffset,count,stride);}
void sharedCmdDrawIndexedIndirectCount(Device* d,PFN_vkCmdDrawIndexedIndirectCount original,bool khr,VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,std::uint32_t count,std::uint32_t stride){
    session().tracker->unknownCommand(d->id,c);unknownCommand(d,c,khr?RecordingOperation::CmdDrawIndexedIndirectCountKHR:RecordingOperation::CmdDrawIndexedIndirectCount);original(c,buffer,offset,counts,countOffset,count,stride);
}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndexedIndirectCount(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,std::uint32_t count,std::uint32_t stride){auto* d=deviceFor(c);sharedCmdDrawIndexedIndirectCount(d,d->CmdDrawIndexedIndirectCount,false,c,buffer,offset,counts,countOffset,count,stride);}
VKAPI_ATTR void VKAPI_CALL traceCmdDrawIndexedIndirectCountKHR(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkBuffer counts,VkDeviceSize countOffset,std::uint32_t count,std::uint32_t stride){auto* d=deviceFor(c);sharedCmdDrawIndexedIndirectCount(d,d->CmdDrawIndexedIndirectCountKHR,true,c,buffer,offset,counts,countOffset,count,stride);}
VKAPI_ATTR void VKAPI_CALL bridgeDestroyImage(VkDevice device,VkImage value,const VkAllocationCallbacks* alloc){
    auto* d=deviceFor(device);d->bridge.resourceRetiring(frame_pixel_ssbo_transaction::Resource::Image,frame_pixel_ssbo_clone::handle(value));traceDestroyImage(device,value,alloc);
}
VKAPI_ATTR void VKAPI_CALL bridgeDestroyImageView(VkDevice device,VkImageView value,const VkAllocationCallbacks* alloc){
    auto* d=deviceFor(device);d->bridge.resourceRetiring(frame_pixel_ssbo_transaction::Resource::View,frame_pixel_ssbo_clone::handle(value));traceDestroyImageView(device,value,alloc);
}
VKAPI_ATTR void VKAPI_CALL bridgeFreeMemory(VkDevice device,VkDeviceMemory value,const VkAllocationCallbacks* alloc){
    auto* d=deviceFor(device);d->bridge.resourceRetiring(frame_pixel_ssbo_transaction::Resource::Memory,frame_pixel_ssbo_clone::handle(value));traceFreeMemory(device,value,alloc);
}
VKAPI_ATTR void VKAPI_CALL bridgeUpdateDescriptorSets(VkDevice device,std::uint32_t count,const VkWriteDescriptorSet* writes,std::uint32_t copies,const VkCopyDescriptorSet* copy){
    auto* d=deviceFor(device);
    if(count>256||copies>256)d->bridge.descriptorMutation();
    else {for(std::uint32_t i=0;i<count;++i)d->bridge.descriptorMutation(writes[i].dstSet);for(std::uint32_t i=0;i<copies;++i)d->bridge.descriptorMutation(copy[i].dstSet);}
    traceUpdateDescriptorSets(device,count,writes,copies,copy);
}
VKAPI_ATTR void VKAPI_CALL bridgeUpdateDescriptorSetWithTemplate(VkDevice device,VkDescriptorSet set,VkDescriptorUpdateTemplate update,const void* data){
    deviceFor(device)->bridge.descriptorMutation(set);traceUpdateDescriptorSetWithTemplate(device,set,update,data);
}
VKAPI_ATTR VkResult VKAPI_CALL bridgeFreeDescriptorSets(VkDevice device,VkDescriptorPool pool,std::uint32_t count,const VkDescriptorSet* sets){
    auto* d=deviceFor(device);if(count>512)d->bridge.descriptorMutation();else for(std::uint32_t i=0;i<count;++i)d->bridge.descriptorMutation(sets[i]);
    return traceFreeDescriptorSets(device,pool,count,sets);
}
VKAPI_ATTR VkResult VKAPI_CALL bridgeResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags){
    auto* d=deviceFor(device);d->bridge.descriptorPoolRetiring(*session().tracker,d->id,pool);return traceResetDescriptorPool(device,pool,flags);
}
VKAPI_ATTR void VKAPI_CALL bridgeDestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks* alloc){
    auto* d=deviceFor(device);d->bridge.descriptorPoolRetiring(*session().tracker,d->id,pool);traceDestroyDescriptorPool(device,pool,alloc);
}
#endif
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
#include "../probe/dashboard_transition_probe_hooks.inc"
#endif
PFN_vkVoidFunction intercept(const char* name,PFN_vkVoidFunction native) {
    if(!native || !session().allowed)return native;
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
#ifdef FRAME_DASHBOARD_TRANSITION_PROBE
    if(auto hooked=dashboardProbeIntercept(name))return hooked;
#endif
    // Functional prototype: only cold definition/lifetime callbacks. Returning
    // the exact NEXT PFN for all recording/draw/submit/resource commands avoids
    // the diagnostic LiveTracker and its per-frame work entirely.
#define PRESENT_HOOK(method) if(!std::strcmp(name,"vk" #method))return reinterpret_cast<PFN_vkVoidFunction>(trace##method);
    PRESENT_HOOK(CreateShaderModule) PRESENT_HOOK(DestroyShaderModule)
    PRESENT_HOOK(CreateGraphicsPipelines) PRESENT_HOOK(DestroyPipeline)
    PRESENT_HOOK(CreateDescriptorSetLayout) PRESENT_HOOK(DestroyDescriptorSetLayout)
    PRESENT_HOOK(CreatePipelineLayout) PRESENT_HOOK(DestroyPipelineLayout)
    PRESENT_HOOK(CreateRenderPass) PRESENT_HOOK(CreateRenderPass2) PRESENT_HOOK(CreateRenderPass2KHR) PRESENT_HOOK(DestroyRenderPass)
#undef PRESENT_HOOK
    return native;
#endif
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    if(!std::strcmp(name,"vkDestroyImage"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeDestroyImage);
    if(!std::strcmp(name,"vkDestroyImageView"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeDestroyImageView);
    if(!std::strcmp(name,"vkFreeMemory"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeFreeMemory);
    if(!std::strcmp(name,"vkUpdateDescriptorSets"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeUpdateDescriptorSets);
    if(!std::strcmp(name,"vkUpdateDescriptorSetWithTemplate")||!std::strcmp(name,"vkUpdateDescriptorSetWithTemplateKHR"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeUpdateDescriptorSetWithTemplate);
    if(!std::strcmp(name,"vkFreeDescriptorSets"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeFreeDescriptorSets);
    if(!std::strcmp(name,"vkResetDescriptorPool"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeResetDescriptorPool);
    if(!std::strcmp(name,"vkDestroyDescriptorPool"))return reinterpret_cast<PFN_vkVoidFunction>(bridgeDestroyDescriptorPool);
#endif
#define HOOK(method) if(!std::strcmp(name,"vk" #method))return reinterpret_cast<PFN_vkVoidFunction>(trace##method);
    HOOK(CreateShaderModule) HOOK(DestroyShaderModule) HOOK(CreateGraphicsPipelines) HOOK(DestroyPipeline)
#ifdef FRAME_PIXEL_SSBO_DEFINITION_JOURNAL
    HOOK(CreateRenderPass) HOOK(CreateRenderPass2) HOOK(DestroyRenderPass)
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    HOOK(GetDeviceQueue) HOOK(GetDeviceQueue2) HOOK(CmdDrawIndirectCount) HOOK(CmdDrawIndexedIndirectCount)
#endif
    HOOK(CreateRenderPass2KHR)
#endif
    HOOK(CreateCommandPool) HOOK(DestroyCommandPool) HOOK(ResetCommandPool)
    HOOK(AllocateCommandBuffers) HOOK(FreeCommandBuffers) HOOK(BeginCommandBuffer) HOOK(EndCommandBuffer) HOOK(ResetCommandBuffer)
    HOOK(CmdBindPipeline) HOOK(CmdDraw) HOOK(CmdDrawIndexed) HOOK(CmdDrawIndirect) HOOK(CmdDrawIndexedIndirect) HOOK(CmdExecuteCommands)
    HOOK(CmdBindShadersEXT)
    HOOK(CmdDispatch) HOOK(CmdDispatchIndirect) HOOK(CmdDispatchBase)
    HOOK(CmdBeginQuery) HOOK(CmdBeginQueryIndexedEXT)
    HOOK(CmdBeginRenderPass) HOOK(CmdNextSubpass) HOOK(CmdEndRenderPass)
    HOOK(CmdBeginRenderPass2) HOOK(CmdNextSubpass2) HOOK(CmdEndRenderPass2) HOOK(CmdBeginRendering) HOOK(CmdEndRendering)
    HOOK(QueueSubmit) HOOK(QueueSubmit2)
#define BINDING_FIELD(method) HOOK(method)
#include "passive_binding_commands.inc"
#undef BINDING_FIELD
#undef HOOK
#define ALIAS(alias,method) if(!std::strcmp(name,alias))return reinterpret_cast<PFN_vkVoidFunction>(trace##method);
#define BINDING_ALIAS(alias,method) ALIAS(alias,method)
#include "passive_binding_aliases.inc"
#undef BINDING_ALIAS
    ALIAS("vkCmdBeginRenderPass2KHR",CmdBeginRenderPass2KHR) ALIAS("vkCmdNextSubpass2KHR",CmdNextSubpass2KHR) ALIAS("vkCmdEndRenderPass2KHR",CmdEndRenderPass2KHR)
    ALIAS("vkCmdBeginRenderingKHR",CmdBeginRenderingKHR) ALIAS("vkCmdEndRenderingKHR",CmdEndRenderingKHR) ALIAS("vkQueueSubmit2KHR",QueueSubmit2KHR)
    ALIAS("vkCmdDispatchBaseKHR",CmdDispatchBaseKHR)
#ifdef FRAME_PIXEL_SSBO_TRANSACTION
    ALIAS("vkCmdDrawIndirectCountKHR",CmdDrawIndirectCountKHR) ALIAS("vkCmdDrawIndexedIndirectCountKHR",CmdDrawIndexedIndirectCountKHR)
#endif
#undef ALIAS
    return native;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL physicalProc(VkInstance instance,const char* name) {
    auto* i=instanceFor(instance);return i && i->gpdpa ? i->gpdpa(instance,name) : nullptr;
}
} // namespace

PIXEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL frame_pixel_vkGetDeviceProcAddr(VkDevice device,const char* name) {
    auto* d=deviceFor(device);if(!d || !name)return nullptr;
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(frame_pixel_vkGetDeviceProcAddr);
    const auto native=d->gdpa(device,name);if(!native)return nullptr;
    if(!std::strcmp(name,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(traceDestroyDevice);
    return intercept(name,native);
}
PIXEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL frame_pixel_vkGetInstanceProcAddr(VkInstance instance,const char* name) {
    if(!name)return nullptr;
    if(!std::strcmp(name,"vkGetInstanceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(frame_pixel_vkGetInstanceProcAddr);
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(frame_pixel_vkGetDeviceProcAddr);
    if(!std::strcmp(name,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(traceCreateInstance);
    auto* i=instanceFor(instance);if(!i)return nullptr;
    const auto native=i->gipa(instance,name);if(!native)return nullptr;
    if(!std::strcmp(name,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(traceDestroyInstance);
    if(!std::strcmp(name,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(traceCreateDevice);
    if(!std::strcmp(name,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(traceDestroyDevice);
    return intercept(name,native);
}
PIXEL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL frame_pixel_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if(!info || info->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion<2)return VK_ERROR_INITIALIZATION_FAILED;
#ifdef FRAME_PIXEL_SYNTHETIC_PRESENTER
    preparePresenterSessionCold();
#endif
    info->loaderLayerInterfaceVersion=std::min(2u,info->loaderLayerInterfaceVersion);
    info->pfnGetInstanceProcAddr=frame_pixel_vkGetInstanceProcAddr;info->pfnGetDeviceProcAddr=frame_pixel_vkGetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr=physicalProc;return VK_SUCCESS;
}
