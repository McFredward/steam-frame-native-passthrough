#include "pixel_ssbo_draw_transaction.h"
#include "submit_timeline_contract.h"

namespace frame_pixel_ssbo_transaction {
namespace {
template<class T>T handle(std::uint64_t value) noexcept {T out{};std::memcpy(&out,&value,sizeof(out));return out;}
bool valid(Id value,std::uint64_t device) noexcept {return value.device==device && value.value && value.generation;}
}
const char* reasonName(Reason value) noexcept {
    switch(value){
#define NAME(x,s) case Reason::x:return s;
    NAME(None,"none") NAME(NotConfigured,"not_configured") NAME(InvalidConfiguration,"invalid_configuration")
    NAME(Contention,"contention") NAME(Identity,"identity") NAME(GenerationWrap,"generation_wrap")
    NAME(Begin,"begin") NAME(NotArmed,"not_armed") NAME(MissingReset,"missing_reset") NAME(Scope,"scope")
    NAME(Prefix,"prefix") NAME(Pipeline,"pipeline") NAME(Source,"source") NAME(EmptyDraw,"empty_draw")
    NAME(LaterDraw,"later_draw") NAME(Subpass,"subpass") NAME(Secondary,"secondary") NAME(UnknownRoute,"unknown_route")
    NAME(CommandEnd,"command_end") NAME(SubmitShape,"submit_shape") NAME(SubmitExtension,"submit_extension") NAME(SubmitBounds,"submit_bounds") NAME(UnknownQueue,"unknown_queue")
    NAME(Overlap,"overlap") NAME(NativeSubmit,"native_submit") NAME(MarkerSubmit,"marker_submit")
    NAME(Fence,"fence") NAME(NotRetired,"not_retired") NAME(CohortMismatch,"cohort_mismatch")
    NAME(SourceRetired,"source_retired") NAME(UnsafeRelease,"unsafe_release") NAME(AlreadyArmed,"already_armed") NAME(DeviceDestroying,"device_destroying")
#undef NAME
    }return "unknown_transaction_refusal";
}
const char* operationName(Operation operation) noexcept {
    switch(operation){
#define OP(name) case Operation::name:return #name;
    OP(Unspecified) OP(BeginCommandBuffer) OP(EndCommandBuffer) OP(CmdBindPipeline) OP(CmdBindDescriptorSets) OP(CmdPushConstants)
    OP(CmdBeginRenderPass) OP(CmdBeginRenderPass2) OP(CmdBeginRenderPass2KHR) OP(CmdNextSubpass) OP(CmdNextSubpass2) OP(CmdNextSubpass2KHR)
    OP(CmdEndRenderPass) OP(CmdEndRenderPass2) OP(CmdEndRenderPass2KHR) OP(CmdDraw) OP(CmdDrawIndexed) OP(CmdDrawIndirect) OP(CmdDrawIndexedIndirect)
    OP(CmdDrawIndirectCount) OP(CmdDrawIndirectCountKHR) OP(CmdDrawIndirectCountAMD) OP(CmdDrawIndexedIndirectCount) OP(CmdDrawIndexedIndirectCountKHR) OP(CmdDrawIndexedIndirectCountAMD)
    OP(CmdExecuteCommands) OP(CmdBindShadersEXT) OP(CmdDispatch) OP(CmdDispatchIndirect) OP(CmdDispatchBase) OP(CmdDispatchBaseKHR)
    OP(CmdBeginQuery) OP(CmdBeginQueryIndexedEXT) OP(CmdBeginRendering) OP(CmdBeginRenderingKHR) OP(CmdEndRendering) OP(CmdEndRenderingKHR)
    OP(QueueSubmit) OP(QueueSubmit2) OP(QueueSubmit2KHR) OP(DescriptorMutation) OP(DescriptorPoolRetiring) OP(ResourceRetiring) OP(DefinitionRetiring)
    OP(ResetCommandBuffer) OP(FreeCommandBuffers) OP(ResetCommandPool) OP(DestroyCommandPool) OP(DeviceDestroying) OP(CopyRecorded) OP(TargetTicket) OP(Count)
#undef OP
    }return "InvalidOperation";
}
const char* phaseName(RecordingPhase phase) noexcept {
    switch(phase){
#define P(name) case RecordingPhase::name:return #name;
    P(Unavailable) P(RecordingOutsidePass) P(OtherPass) P(BeforeTargetSubpass) P(TargetSubpassBeforeDraw) P(TargetSubpassAfterDraw) P(AfterTargetSubpass) P(AfterPass) P(Executable) P(Retired)
#undef P
    }return "InvalidPhase";
}
namespace {
void normalizeContext(RecordingContext& context) noexcept {
    if(static_cast<unsigned>(context.input.operation)>=static_cast<unsigned>(Operation::Count)){context.input.operation=Operation::Unspecified;context.metadataTruncated=true;}
    if(static_cast<unsigned>(context.input.source)>static_cast<unsigned>(ContextSource::Internal)){context.input.source=ContextSource::Unspecified;context.metadataTruncated=true;}
    for(auto& chain:context.input.chains)if(chain.count>chain.types.size()){chain.count=chain.types.size();context.metadataTruncated=true;}
}
}
RecordingPhase Transaction::contextPhase() const noexcept {
    if(retired_)return RecordingPhase::Retired;
    if(!recording_)return executable_?RecordingPhase::Executable:RecordingPhase::Unavailable;
    if(!inPass_)return scopeEnded_?RecordingPhase::AfterPass:RecordingPhase::RecordingOutsidePass;
    if(!targetPass_)return RecordingPhase::OtherPass;
    if(currentSubpass_<targetSubpass_)return RecordingPhase::BeforeTargetSubpass;
    if(currentSubpass_>targetSubpass_)return RecordingPhase::AfterTargetSubpass;
    return selected_?RecordingPhase::TargetSubpassAfterDraw:RecordingPhase::TargetSubpassBeforeDraw;
}
Transaction::ContextScope::ContextScope(Transaction& owner,VkCommandBuffer command,const CommandContextInput& input,Operation fallback,bool rejected) noexcept:owner_(owner) {
    if(!owner.matches(command)||owner.retired_)return;
    enabled_=true;context_.input=input;
    if(context_.input.operation==Operation::Unspecified){context_.input.operation=fallback;context_.input.source=ContextSource::Internal;}
    normalizeContext(context_);
    context_.phase=owner.contextPhase();context_.before=owner.refusal();context_.rejected=rejected;
    context_.transactionEpoch=owner.epoch_;context_.device=frame_pixel_ssbo_clone::handle(owner.device_);
    context_.command=frame_pixel_ssbo_clone::handle(command);context_.pool=frame_pixel_ssbo_clone::handle(owner.pool_);
    context_.commandGeneration=owner.generation_;context_.poolGeneration=owner.poolGeneration_;context_.drawOrdinal=owner.drawOrdinal_;
    context_.currentSubpass=owner.currentSubpass_;context_.targetSubpass=owner.targetSubpass_;context_.subpassCount=owner.subpassCount_;
}
Transaction::ContextScope::~ContextScope(){
    if(!enabled_)return;
    context_.after=owner_.refusal();context_.rejected|=context_.before==Reason::None&&context_.after!=Reason::None;
    owner_.retainContext(context_);
}
void Transaction::firstContext(const RecordingContext& context,bool ownsContention) noexcept {
    // A later event that merely observes an earlier sticky reason is not its
    // cause. Leave the first-context slot explicitly unavailable in that case.
    // A Guard owner can observe concurrent Contention at scope exit, but only
    // the losing Guard that won that refusal CAS owns its causal context.
    if(!context.rejected||context.before!=Reason::None||context.after==Reason::None||
       (context.after==Reason::Contention&&!ownsContention))return;
    unsigned empty=0;if(!firstContextState_.compare_exchange_strong(empty,1,std::memory_order_acq_rel))return;
    firstContext_=context;firstContextState_.store(2,std::memory_order_release);
}
void Transaction::contextContention(VkCommandBuffer command,const CommandContextInput& input,bool firstRefusal) noexcept {
    contextContended_.store(true,std::memory_order_release);
    RecordingContext context;context.input=input;context.command=frame_pixel_ssbo_clone::handle(command);context.transactionEpoch=epoch_;
    // No plain mutable transaction state is read without Guard. Incomplete
    // generation/phase is explicit, never assembled from unrelated atomics.
    context.after=Reason::Contention;context.rejected=true;context.metadataTruncated=true;normalizeContext(context);
    if(firstRefusal&&refusal()==Reason::Contention)firstContext(context,true);
}
namespace {
bool sameContext(const RecordingContext& a,const RecordingContext& b) noexcept {
    const auto& x=a.input;const auto& y=b.input;
    if(a.transactionEpoch!=b.transactionEpoch||a.command!=b.command||a.pool!=b.pool||a.commandGeneration!=b.commandGeneration||a.poolGeneration!=b.poolGeneration||
       a.phase!=b.phase||a.currentSubpass!=b.currentSubpass||a.targetSubpass!=b.targetSubpass||a.subpassCount!=b.subpassCount||a.before!=b.before||a.after!=b.after||a.rejected!=b.rejected||a.metadataTruncated!=b.metadataTruncated||
       x.operation!=y.operation||x.source!=y.source||x.detailReason!=y.detailReason||x.structureType!=y.structureType||x.index!=y.index||x.chain!=y.chain||x.contents!=y.contents||x.flags!=y.flags||x.targetPipeline!=y.targetPipeline||x.privateProfile!=y.privateProfile||x.privateSrgbMask!=y.privateSrgbMask||x.privatePayloadObserved!=y.privatePayloadObserved)return false;
    // Object values and draw ordinals are first-observation payload, not a key:
    // ordinary binds/draws must not starve the independent first refusal.
    for(unsigned i=0;i<x.chains.size();++i){const auto& p=x.chains[i];const auto& q=y.chains[i];if(p.rootType!=q.rootType||p.count!=q.count||p.termination!=q.termination)return false;for(unsigned n=0;n<p.count;++n)if(p.types[n]!=q.types[n])return false;}
    return true;
}
}
void Transaction::retainContext(RecordingContext context) noexcept {
    const auto old=contextAttempts_.load(std::memory_order_relaxed);
    if(old==UINT64_MAX){contextOverflow_.store(UINT64_MAX,std::memory_order_relaxed);firstContext(context);return;}
    context.sequence=old+1;contextAttempts_.store(context.sequence,std::memory_order_release);firstContext(context);
    const auto count=contextCount_.load(std::memory_order_relaxed);
    for(unsigned i=0;i<count;++i)if(sameContext(contexts_[i].first,context)){
        const auto repeats=contexts_[i].count.load(std::memory_order_relaxed);if(repeats!=UINT64_MAX)contexts_[i].count.store(repeats+1,std::memory_order_release);
        contexts_[i].lastSequence.store(context.sequence,std::memory_order_release);return;
    }
    if(count==contexts_.size()){const auto overflow=contextOverflow_.load(std::memory_order_relaxed);if(overflow!=UINT64_MAX)contextOverflow_.store(overflow+1,std::memory_order_release);return;}
    auto& slot=contexts_[count];slot.first=context;slot.count.store(1,std::memory_order_relaxed);slot.lastSequence.store(context.sequence,std::memory_order_relaxed);
    contextCount_.store(count+1,std::memory_order_release); // immutable payload publication
}
RecordingContexts Transaction::recordingContexts() const noexcept {
    RecordingContexts out;out.count=contextCount_.load(std::memory_order_acquire);
    for(unsigned i=0;i<out.count;++i){out.entries[i].first=contexts_[i].first;out.entries[i].count=contexts_[i].count.load(std::memory_order_acquire);out.entries[i].lastSequence=contexts_[i].lastSequence.load(std::memory_order_acquire);}
    out.attempts=contextAttempts_.load(std::memory_order_acquire);out.overflow=contextOverflow_.load(std::memory_order_acquire);out.contended=contextContended_.load(std::memory_order_acquire);
    out.firstRefusalAvailable=firstContextState_.load(std::memory_order_acquire)==2;if(out.firstRefusalAvailable)out.firstRefusal=firstContext_;
    return out;
}
void Transaction::observe(VkCommandBuffer command,const CommandContextInput& input) noexcept {
    Guard guard(*this);if(!guard){contextContention(command,input,guard.firstRefusal());return;}
    ContextScope context(*this,command,input,Operation::Unspecified);
}
std::uint64_t Transaction::reserveEpoch() noexcept {
    auto value=epochs_.load(std::memory_order_relaxed);
    while(value && value!=UINT64_MAX){if(epochs_.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;}
    return 0; // Sticky process-lifetime exhaustion; never wrap into an old epoch.
}
Transaction::Transaction() noexcept:epoch_(reserveEpoch()) {if(!epoch_)refuse(Reason::GenerationWrap);}
Transaction::Guard::Guard(Transaction& owner) noexcept:owner_(owner),acquired_(!owner.lock_.test_and_set(std::memory_order_acquire)) {
    if(!acquired_){auto expected=Reason::None;firstRefusal_=owner_.reason_.compare_exchange_strong(expected,Reason::Contention,std::memory_order_acq_rel);owner_.contextContended_.store(true,std::memory_order_release);}
}
Transaction::Guard::~Guard(){if(acquired_)owner_.lock_.clear(std::memory_order_release);}
bool Transaction::refuse(Reason value) noexcept {
    auto expected=Reason::None;reason_.compare_exchange_strong(expected,value,std::memory_order_acq_rel);return false;
}
bool Transaction::advance(std::uint64_t& value) noexcept {return value!=UINT64_MAX?++value!=0:refuse(Reason::GenerationWrap);}
bool Transaction::same(Id a,Id b) noexcept{return a.device==b.device && a.value==b.value && a.generation && a.generation==b.generation;}
bool Transaction::same(const Cohort& a,const Cohort& b) noexcept {
    return same(a.image,b.image) && same(a.view,b.view) && same(a.memory,b.memory) && a.layout==b.layout &&
        a.extent.width && a.extent.height && a.extent.width==b.extent.width && a.extent.height==b.extent.height;
}
bool Transaction::matches(VkCommandBuffer command) const noexcept{return device_ && command_ && command==command_;}
bool Transaction::active(VkCommandBuffer command) noexcept {
    return refusal()==Reason::None && matches(command) && recording_ && !retired_;
}
bool Transaction::deviceCreated(VkDevice device,const VkDeviceCreateInfo& input,VkResult nativeResult) noexcept {
    Guard guard(*this);if(!guard || nativeResult!=VK_SUCCESS)return false;
    if(device_ || !device || input.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || input.flags ||
       input.queueCreateInfoCount!=1 || !input.pQueueCreateInfos)return refuse(Reason::InvalidConfiguration);
    const auto& q=input.pQueueCreateInfos[0];
    if(q.sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || q.flags || q.queueCount!=1 ||
       !q.pQueuePriorities || q.queueFamilyIndex==VK_QUEUE_FAMILY_IGNORED || !std::isfinite(q.pQueuePriorities[0]) ||
       q.pQueuePriorities[0]<0 || q.pQueuePriorities[0]>1)return refuse(Reason::InvalidConfiguration);
    if(q.pNext){
        const auto* node=static_cast<const VkBaseInStructure*>(q.pNext);
        if(node->sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR || node->pNext)
            return refuse(Reason::InvalidConfiguration);
        const auto priority=reinterpret_cast<const VkDeviceQueueGlobalPriorityCreateInfoKHR*>(node)->globalPriority;
        if(priority!=VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR && priority!=VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR &&
           priority!=VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR && priority!=VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR)
            return refuse(Reason::InvalidConfiguration);
        // The ORIGINAL successful create already selected this scheduling
        // priority. It changes neither queue identity nor marker ordering; do
        // not reject an ordinary high-priority compositor queue or rewrite it.
    }
    device_=device;family_=q.queueFamilyIndex;return epoch_!=0;
}
bool Transaction::queueRetrieved(VkDevice device,std::uint32_t family,std::uint32_t index,VkQueue queue) noexcept {
    Guard guard(*this);if(!guard)return false;
    if(!device_ || device!=device_ || family!=family_ || index || !queue || (queue_ && queue_!=queue))return refuse(Reason::UnknownQueue);
    queue_=queue;return true;
}
bool Transaction::configure(frame_pixel_ssbo_owned::Clone& clone,const Configuration& config) noexcept {
    Guard guard(*this);if(!guard)return false;
    const auto* prepared=clone.preparedRecipe();
    if(!prepared)return refuse(Reason::InvalidConfiguration);
    const auto& recipe=*prepared;const auto& p=config.dispatch;const auto device=frame_pixel_ssbo_clone::handle(device_);
    if(refusal()!=Reason::None || configured_ || !device_ || !queue_ || family_==VK_QUEUE_FAMILY_IGNORED || !clone.ready() ||
       clone.referenced() || !recipe.definitionComplete || !recipe.set0.complete || !recipe.layout.complete ||
       !recipe.renderPass.complete || !recipe.renderPass.subpassCount || recipe.renderPass.subpassCount>frame_pixel_ssbo_clone::MaxSubpasses ||
       recipe.graphics.info.subpass>=recipe.renderPass.subpassCount || recipe.runtimeActivationAuthorized || recipe.nativeLifetimeProven ||
       !p.barrier || !p.fill || !p.pipeline || !p.sets || !p.draw || !p.drawIndexed || !p.createFence || !p.destroyFence ||
       !p.resetFence || !p.fenceStatus || !p.submit || !p.invalidate)return refuse(Reason::InvalidConfiguration);
    for(const auto& id:recipe.provenance)if(!valid(id,device))return refuse(Reason::Identity);
    // The prepared Recipe is the immutable snapshot ACTUALLY used to build
    // this Clone, not a different caller-passed bag of all-true facts. It is CPU
    // definition history, still NOT native object lifetime or frame/eye proof.
    VkFenceCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;VkFence created=VK_NULL_HANDLE;
    const auto result=p.createFence(device_,&create,nullptr,&created);
    if(result!=VK_SUCCESS || !created)return refuse(Reason::Fence); // failed output is undefined, never destroy it
    config_=config;clone_=&clone;fence_=created;set0_=recipe.set0;prefix_=recipe.layout;provenance_=recipe.provenance;
    original_=handle<VkPipeline>(provenance_[5].value);originalLayout_=handle<VkPipelineLayout>(provenance_[1].value);
    pass_=handle<VkRenderPass>(provenance_[2].value);targetSubpass_=recipe.graphics.info.subpass;
    subpassCount_=recipe.renderPass.subpassCount;configured_=true;return true;
}
bool Transaction::poolCreated(VkCommandPool pool,std::uint32_t family,VkResult result) noexcept {
    Guard guard(*this);if(!guard)return false;
    if(!device_)return refuse(Reason::NotConfigured);
    if(result!=VK_SUCCESS)return false;
    if(pool_ || !pool || family!=family_)return refuse(Reason::Identity);
    pool_=pool;return advance(poolGeneration_);
}
bool Transaction::commandAllocated(VkCommandPool pool,VkCommandBuffer command,VkCommandBufferLevel level,VkResult result) noexcept {
    Guard guard(*this);if(!guard)return false;if(result!=VK_SUCCESS)return false;
    if(!device_ || pool!=pool_ || !pool_ || command_ || !command || level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY)
        return refuse(Reason::Identity);
    command_=command;return true;
}
bool Transaction::beginSucceeded(VkCommandBuffer command,const VkCommandBufferBeginInfo& begin,VkResult result) noexcept {
    Guard guard(*this);if(!guard)return false;if(result!=VK_SUCCESS)return false;
    if(command!=command_ || !command_ || recording_ || armed_ || begin.sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO ||
       begin.pNext || begin.pInheritanceInfo || (begin.flags&~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))return refuse(Reason::Begin);
    if(!advance(generation_))return false;
    recording_=true;retired_=false;inPass_=false;scopeEnded_=false;selected_=false;
    current_=VK_NULL_HANDLE;currentSet0_=VK_NULL_HANDLE;setIdentity_={};currentSource_={};selectedSource_={};
    drawOrdinal_=selectedOrdinal_=0;currentSubpass_=0;prefixValid_=mirrorValidated_=false;matchingPass_=targetPass_=false;cleanHistory_=true;return true;
}
bool Transaction::adoptRecording(const RecordingTicket& ticket) noexcept {
    Guard guard(*this);if(!guard)return false;
    if(!configured_ || armed_ || !ticket.tracker_ || ticket.device_!=frame_pixel_ssbo_clone::handle(device_) ||
       !ticket.command_ || !ticket.pool_ || !ticket.commandGeneration_ || !ticket.poolGeneration_ || ticket.family_!=family_ ||
       (ticket.flags_&~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) ||
       (adoptedTracker_ && adoptedTracker_!=ticket.tracker_) ||
       (command_==ticket.command_ && generation_>=ticket.commandGeneration_))return refuse(Reason::Identity);
    adoptedTracker_=ticket.tracker_;command_=ticket.command_;pool_=ticket.pool_;
    generation_=ticket.commandGeneration_;poolGeneration_=ticket.poolGeneration_;
    recording_=true;retired_=false;inPass_=matchingPass_=targetPass_=false;scopeEnded_=selected_=false;currentSubpass_=0;
    current_=VK_NULL_HANDLE;currentSet0_=VK_NULL_HANDLE;setIdentity_={};currentSource_={};selectedSource_={};
    drawOrdinal_=selectedOrdinal_=0;prefixValid_=mirrorValidated_=false;cleanHistory_=true;return true;
}
bool Transaction::adoptTargetState(const TargetTicket& ticket) noexcept {
    Guard guard(*this);if(!guard)return false;
    ContextScope context(*this,ticket.command_,{},Operation::TargetTicket);
    if(!configured_ || !armed_ || !active(ticket.command_) || !inPass_ || !matchingPass_ || selected_ ||
       !adoptedTracker_ || ticket.tracker_!=adoptedTracker_ || ticket.device_!=frame_pixel_ssbo_clone::handle(device_) ||
       ticket.commandGeneration_!=generation_ || ticket.pool_!=pool_ || ticket.poolGeneration_!=poolGeneration_ ||
       ticket.ordinal_!=drawOrdinal_+1 || ticket.subpass_!=currentSubpass_ || ticket.subpass_!=targetSubpass_ ||
       !same(ticket.pipeline_,provenance_[5]) || !same(ticket.layout_,provenance_[1]) ||
       !valid(ticket.set0_,ticket.device_) || !valid(ticket.source_.image,ticket.device_) ||
       !valid(ticket.source_.view,ticket.device_) || !valid(ticket.source_.memory,ticket.device_) ||
       ticket.source_.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || !same(ticket.source_,ticket.source_))return refuse(Reason::Identity);
    mirrorValidated_=false;
    if(clone_->compatibleSet0Mirror()){
        std::uint32_t expected=0;for(unsigned i=0;i<set0_.count;++i)if(set0_.bindings[i].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || set0_.bindings[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)expected+=set0_.bindings[i].descriptorCount;
        // Core restore identity and Journal creation identity have distinct
        // generation namespaces. The opaque producer proves Core association;
        // only the actual device/raw layout value is common to both domains.
        if(!valid(ticket.restoreLayout_,ticket.device_)||ticket.restoreLayout_.value!=provenance_[1].value||ticket.restoreCount_!=expected||expected>restoreOffsets_.size())return refuse(Reason::Prefix);
        restoreLayout_=ticket.restoreLayout_;restoreCount_=ticket.restoreCount_;restoreOffsets_=ticket.restoreOffsets_;
    }
    current_=original_;currentSet0_=handle<VkDescriptorSet>(ticket.set0_.value);setIdentity_=ticket.set0_;
    currentSource_=ticket.source_;prefixValid_=true;return true;
}
bool Transaction::targetStateCurrent(const TargetTicket& ticket) noexcept {
    Guard guard(*this);if(!guard)return false;
    ContextScope context(*this,ticket.command_,{},Operation::TargetTicket);
    if(!configured_||!clone_->compatibleSet0Mirror()||!clone_->mirrorPopulated()||!active(ticket.command_)||selected_||!prefixValid_||
       !inPass_||!matchingPass_||current_!=original_||!adoptedTracker_||ticket.tracker_!=adoptedTracker_||ticket.device_!=frame_pixel_ssbo_clone::handle(device_)||
       ticket.commandGeneration_!=generation_||ticket.pool_!=pool_||ticket.poolGeneration_!=poolGeneration_||
       ticket.ordinal_!=drawOrdinal_+1||ticket.subpass_!=currentSubpass_||ticket.subpass_!=targetSubpass_||
       !same(ticket.pipeline_,provenance_[5])||!same(ticket.layout_,provenance_[1])||!same(ticket.set0_,setIdentity_)||
       !same(ticket.source_,currentSource_)||!same(ticket.restoreLayout_,restoreLayout_)||ticket.restoreCount_!=restoreCount_)return refuse(Reason::Prefix);
    for(unsigned i=0;i<restoreCount_;++i)if(ticket.restoreOffsets_[i]!=restoreOffsets_[i])return refuse(Reason::Prefix);
    mirrorValidated_=true;return true;
}
bool Transaction::arm(VkCommandBuffer command) noexcept {
    Guard guard(*this);if(!guard)return false;
    ContextScope context(*this,command,{},Operation::BeginCommandBuffer);
    if(armed_)return refuse(Reason::AlreadyArmed);
    if(!configured_ || !active(command) || inPass_ || scopeEnded_ || drawOrdinal_ || !cleanHistory_ || selected_ || !generation_)return refuse(Reason::NotArmed);
    if(!clone_->ready() || clone_->referenced() || !clone_->markReferenced())return refuse(Reason::InvalidConfiguration);
    armed_=true;
    // This GPU reset is inside the ORIGINAL reusable recording, before its
    // render pass. It therefore executes on EVERY serial replay; a CPU memset
    // or per-recording initialization cannot accidentally authorize old data.
    VkBufferMemoryBarrier before{};before.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;before.buffer=clone_->recordBuffer();before.size=frame_pixel_ssbo_owned::RecordBytes;
    before.srcQueueFamilyIndex=before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    config_.dispatch.barrier(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&before,0,nullptr);
    config_.dispatch.fill(command,clone_->recordBuffer(),0,frame_pixel_ssbo_owned::RecordBytes,0);
    auto after=before;after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    config_.dispatch.barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,nullptr,1,&after,0,nullptr);reset_=true;return refusal()==Reason::None;
}
bool Transaction::beginRenderPass(VkCommandBuffer command,VkRenderPass pass,VkSubpassContents contents) noexcept {
    Guard guard(*this);if(!guard)return false;
    ContextScope context(*this,command,{},Operation::CmdBeginRenderPass);
    if(!active(command))return false;
    if(inPass_){if(armed_)return refuse(Reason::Scope);cleanHistory_=false;return false;}
    inPass_=true;
    if(!armed_){cleanHistory_=false;return false;}
    if(!reset_)return refuse(Reason::MissingReset);
    if((selected_ || scopeEnded_)&&!clone_->compatibleSet0Mirror())return refuse(Reason::Scope);
    if(contents!=VK_SUBPASS_CONTENTS_INLINE)return refuse(Reason::Secondary);
    currentSubpass_=0;targetPass_=pass==pass_;
    matchingPass_=targetPass_ && currentSubpass_==targetSubpass_;
    return targetPass_;
}
void Transaction::pipelineBound(VkCommandBuffer command,VkPipeline pipeline) noexcept {
    Guard guard(*this);if(!guard || !active(command))return;current_=pipeline;
}
void Transaction::set0Bound(VkCommandBuffer command,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,
                           const VkDescriptorSet* sets,std::uint32_t dynamicOffsets,Id descriptorIdentity) noexcept {
    Guard guard(*this);if(!guard || !active(command))return;
    if(!configured_){currentSet0_=VK_NULL_HANDLE;setIdentity_={};return;}
    if(layout!=originalLayout_ || first || count!=1 || !sets || !sets[0] || dynamicOffsets || !valid(descriptorIdentity,frame_pixel_ssbo_clone::handle(device_)) ||
       descriptorIdentity.value!=frame_pixel_ssbo_clone::handle(sets[0])){prefixValid_=false;return;}
    currentSet0_=sets[0];setIdentity_=descriptorIdentity;prefixValid_=true;
}
void Transaction::pushConstants(VkCommandBuffer command,VkPipelineLayout layout,VkShaderStageFlags stages,
                               std::uint32_t offset,std::uint32_t bytes,const void* data) noexcept {
    Guard guard(*this);if(!guard || !active(command))return;
    if(!configured_)return;
    if(layout!=originalLayout_ || !data || !bytes || bytes%4 || offset%4 || offset>4096 || bytes>4096-offset || !stages){prefixValid_=false;return;}
    VkShaderStageFlags covered=0;
    for(std::uint32_t i=0;i<prefix_.count;++i){const auto& r=prefix_.ranges[i];
        if(offset>=r.offset && bytes<=r.size && offset-r.offset<=r.size-bytes)covered|=r.stageFlags;}
    if((covered&stages)!=stages)prefixValid_=false;
}
void Transaction::sourceBound(VkCommandBuffer command,const Cohort& source) noexcept {
    Guard guard(*this);if(!guard || !active(command))return;
    const auto device=frame_pixel_ssbo_clone::handle(device_);
    if(!valid(source.image,device) || !valid(source.view,device) || !valid(source.memory,device) ||
       !source.extent.width || !source.extent.height || source.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){refuse(Reason::Source);return;}
    if(selected_ && !same(source,selectedSource_)){refuse(Reason::Source);return;}currentSource_=source;
}
DrawResult Transaction::draw(VkCommandBuffer command,std::uint32_t vertices,std::uint32_t instances,
                            std::uint32_t firstVertex,std::uint32_t firstInstance) noexcept {
    return replace(command,vertices,instances,firstVertex,firstInstance,0,false);
}
DrawResult Transaction::drawIndexed(VkCommandBuffer command,std::uint32_t indices,std::uint32_t instances,
                                   std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance) noexcept {
    return replace(command,indices,instances,firstIndex,firstInstance,vertexOffset,true);
}
DrawResult Transaction::replace(VkCommandBuffer command,std::uint32_t vertices,std::uint32_t instances,
                               std::uint32_t firstVertex,std::uint32_t firstInstance,std::int32_t vertexOffset,bool indexed) noexcept {
    Guard guard(*this);if(!guard)return DrawResult::ForwardOriginal;
    ContextScope context(*this,command,{},indexed?Operation::CmdDrawIndexed:Operation::CmdDraw);
    if(!active(command))return DrawResult::ForwardOriginal;
    if(!advance(drawOrdinal_))return DrawResult::ForwardOriginal;
    if(selected_){if(!clone_->compatibleSet0Mirror())refuse(Reason::LaterDraw);return DrawResult::ForwardOriginal;}
    if(!armed_)return DrawResult::ForwardOriginal;
    if(!reset_){refuse(Reason::MissingReset);return DrawResult::ForwardOriginal;}
    if(!inPass_){refuse(Reason::Scope);return DrawResult::ForwardOriginal;}
    // Preceding ordinary unrelated native draws are preserved, including the
    // real receiver target at ordinal 4. Only the exact target profile is
    // replaced. The compatible mirror restores native state before any suffix;
    // the legacy Set1 control still refuses subsequent draws.
    if(current_!=original_ || !matchingPass_)return DrawResult::ForwardOriginal;
    if(!vertices || !instances){refuse(Reason::EmptyDraw);return DrawResult::ForwardOriginal;}
    if(!currentSet0_ || !prefixValid_){refuse(Reason::Prefix);return DrawResult::ForwardOriginal;}
    if(!same(currentSource_,currentSource_)){refuse(Reason::Source);return DrawResult::ForwardOriginal;}
    if(!clone_->ready() || !clone_->referenced()){refuse(Reason::InvalidConfiguration);return DrawResult::ForwardOriginal;}
    const bool mirror=clone_->compatibleSet0Mirror();
    if(mirror && (!mirrorValidated_||!clone_->mirrorPopulated()||!valid(restoreLayout_,frame_pixel_ssbo_clone::handle(device_))||!clone_->markMirrorBound())){refuse(Reason::Prefix);return DrawResult::ForwardOriginal;}
    selected_=true;selectedOrdinal_=drawOrdinal_;selectedSource_=currentSource_;
    const auto set=mirror?clone_->mirrorSet():clone_->set1();
    config_.dispatch.sets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,clone_->layout(),mirror?0u:1u,1,&set,mirror?restoreCount_:0,mirror&&restoreCount_?restoreOffsets_.data():nullptr);
    config_.dispatch.pipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,clone_->pipeline());
    if(indexed)config_.dispatch.drawIndexed(command,vertices,instances,firstVertex,vertexOffset,firstInstance);
    else config_.dispatch.draw(command,vertices,instances,firstVertex,firstInstance);
    // MUST be the next command even if a downstream callback reveals reentry or
    // poisons publication. Restoring is cleanup, never conditional authorization.
    config_.dispatch.pipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,original_);
    if(mirror)config_.dispatch.sets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,handle<VkPipelineLayout>(restoreLayout_.value),0,1,&currentSet0_,restoreCount_,restoreCount_?restoreOffsets_.data():nullptr);
    return DrawResult::ReplacedOriginal;
}
void Transaction::endRenderPass(VkCommandBuffer command) noexcept {
    Guard guard(*this);if(!guard)return;
    ContextScope context(*this,command,{},Operation::CmdEndRenderPass);
    if(!active(command))return;
    if(!armed_){inPass_=false;scopeEnded_=true;return;}
    if(!inPass_){refuse(Reason::Scope);return;}
    if(targetPass_ && currentSubpass_+1!=subpassCount_){refuse(Reason::Subpass);return;}
    inPass_=false;matchingPass_=targetPass_=false;
    if(selected_&&!scopeEnded_){
        scopeEnded_=true;
        if(!clone_->ready()){refuse(Reason::InvalidConfiguration);return;}
        VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        host.buffer=clone_->recordBuffer();host.size=frame_pixel_ssbo_owned::RecordBytes;
        config_.dispatch.barrier(command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    }
}
void Transaction::endCommand(VkCommandBuffer command,VkResult result) noexcept {
    Guard guard(*this);if(!guard || !matches(command))return;
    ContextScope context(*this,command,{},Operation::EndCommandBuffer);
    if(!armed_){recording_=false;inPass_=false;return;}
    if(result!=VK_SUCCESS || !recording_ || inPass_ || !scopeEnded_ || !selected_){refuse(Reason::CommandEnd);return;}
    recording_=false;executable_=true;
}
void Transaction::unknown(VkCommandBuffer command,Reason why,const CommandContextInput& input) noexcept {
    Guard guard(*this);if(!guard){contextContention(command,input,guard.firstRefusal());return;}
    ContextScope context(*this,command,input,Operation::Unspecified,true);
    if(matches(command) && !retired_){cleanHistory_=false;if(armed_)refuse(why);}
}
void Transaction::laterDraw(VkCommandBuffer command) noexcept {
    Guard guard(*this);if(!guard || !matches(command) || retired_)return;
    if(advance(drawOrdinal_) && selected_&&!clone_->compatibleSet0Mirror())refuse(Reason::LaterDraw);
}
bool Transaction::nextSubpass(VkCommandBuffer c,VkSubpassContents contents) noexcept {
    Guard guard(*this);if(!guard)return false;
    ContextScope context(*this,c,{},Operation::CmdNextSubpass);
    if(!active(c))return false;
    if(!inPass_)return refuse(Reason::Scope);
    if(contents!=VK_SUBPASS_CONTENTS_INLINE)return refuse(Reason::Secondary);
    // An unrelated preceding pass has no captured shape and grants no target
    // authority. Bound its observed transitions without inventing its count.
    const auto count=targetPass_?subpassCount_:std::uint32_t(frame_pixel_ssbo_clone::MaxSubpasses);
    if(currentSubpass_+1>=count)return refuse(Reason::Subpass);
    ++currentSubpass_;matchingPass_=targetPass_ && currentSubpass_==targetSubpass_;
    return true;
}
void Transaction::executeSecondary(VkCommandBuffer c) noexcept{CommandContextInput context;context.operation=Operation::CmdExecuteCommands;context.source=ContextSource::Internal;unknown(c,Reason::Secondary,context);}
void Transaction::unknownRoute(VkCommandBuffer c,const CommandContextInput& context) noexcept{unknown(c,Reason::UnknownRoute,context);}
void Transaction::resourceRetiring(Resource kind,Id id) noexcept {
    Guard guard(*this);if(!guard || !device_)return;
    for(const auto& source:{currentSource_,selectedSource_}){
        const bool hit=(kind==Resource::Image && same(id,source.image)) || (kind==Resource::View && same(id,source.view)) ||
            (kind==Resource::Memory && same(id,source.memory));
        if(hit){if(armed_)refuse(Reason::SourceRetired);else cleanHistory_=false;return;}
    }
    if(kind==Resource::DescriptorSet && same(id,setIdentity_)){if(armed_)refuse(Reason::Identity);else cleanHistory_=false;return;}
    // Shader/SetLayout/PipelineLayout provenance is consumed definition history,
    // not live borrowed storage. Never confuse equal raw handles of DIFFERENT
    // Vulkan resource kinds. Original pipeline/renderpass remain dependencies.
    if((kind==Resource::Pipeline && same(id,provenance_[5])) || (kind==Resource::RenderPass && same(id,provenance_[2]))){
        if(armed_)refuse(Reason::Identity);else cleanHistory_=false;
    }
}
bool Transaction::fenceComplete() noexcept {
    if(!submitted_ || !pending_)return complete_;
    const auto result=config_.dispatch.fenceStatus(device_,fence_);
    if(result==VK_NOT_READY)return false;
    if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST)deviceLost_=true;return refuse(Reason::Fence);}
    pending_=false;complete_=true;return true;
}
Transaction::SubmitTicket Transaction::beforeSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits) noexcept {
    Guard guard(*this);SubmitTicket ticket;if(!guard || !configured_)return ticket;
    quiescent_=false; // ANY observed new native submission invalidates an earlier idle observation.
    if(refusal()!=Reason::None)return ticket;
    if(queue!=queue_){refuse(Reason::UnknownQueue);return ticket;}
    if(!armed_)return ticket;
    // Inspect the ORIGINAL untouched batch, without allocating/copying its
    // arrays. Unrelated buffers remain native; only a single occurrence of the
    // selected original recording can authorize the one owned completion marker.
    submitObservation_={};submitObservation_.batches=count;
    if(count>32){refuse(Reason::SubmitBounds);return ticket;}
    if(count && !submits){refuse(Reason::SubmitShape);return ticket;}
    std::uint32_t occurrences=0;
    for(std::uint32_t i=0;i<count;++i){const auto& info=submits[i];
        submitObservation_.batch=i;submitObservation_.commandCount=info.commandBufferCount;
        if(info.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO){refuse(Reason::SubmitShape);return ticket;}
        if(info.commandBufferCount>64 || info.waitSemaphoreCount>64 || info.signalSemaphoreCount>64){refuse(Reason::SubmitBounds);return ticket;}
        if((info.commandBufferCount && !info.pCommandBuffers) ||
           (info.waitSemaphoreCount && (!info.pWaitSemaphores || !info.pWaitDstStageMask)) ||
           (info.signalSemaphoreCount && !info.pSignalSemaphores)){refuse(Reason::SubmitShape);return ticket;}
        if(info.pNext){
            const auto timeline=stream_bridge_submit::canonicalTimeline(info,submitObservation_.nextType);
            if(timeline==stream_bridge_submit::TimelineStatus::Extension){refuse(Reason::SubmitExtension);return ticket;}
            if(timeline==stream_bridge_submit::TimelineStatus::Shape){refuse(Reason::SubmitShape);return ticket;}
        }
        for(std::uint32_t n=0;n<info.commandBufferCount;++n)if(info.pCommandBuffers[n]==command_){
            ++occurrences;submitObservation_.selectedOccurrences=occurrences;
            if(occurrences>1){refuse(Reason::SubmitShape);return ticket;}
        }
    }
    if(!occurrences)return ticket; // A valid unrelated original submit is not optional-work failure.
    if(!executable_ || retired_ || pendingSubmit_ || pendingRetirement_){refuse(Reason::Overlap);return ticket;}
    if(pending_ && !fenceComplete()){if(refusal()==Reason::None)refuse(Reason::Overlap);return ticket;}
    if(!advance(submitNonce_))return ticket;
    pendingSubmit_=submitNonce_;ticket.owner_=this;ticket.epoch_=epoch_;ticket.generation_=generation_;ticket.nonce_=submitNonce_;return ticket;
}
bool Transaction::afterSubmit(const SubmitTicket& ticket,VkResult result) noexcept {
    Guard guard(*this);if(!guard)return false;
    if(ticket.owner_!=this || ticket.epoch_!=epoch_ || ticket.generation_!=generation_ || !ticket.nonce_ || ticket.nonce_!=pendingSubmit_)
        return false; // Stale/foreign/replayed tickets cannot consume a newer admission.
    pendingSubmit_=0;
    if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST)deviceLost_=true;return refuse(Reason::NativeSubmit);}
    // An empty marker follows the UNMODIFIED native submit on the same original
    // externally synchronized queue. It waits for prior queue execution only;
    // this is not command retirement or a source video-frame association.
    if(refusal()!=Reason::None)return false;
    result=config_.dispatch.resetFence(device_,1,&fence_);
    if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST)deviceLost_=true;return refuse(Reason::Fence);}
    VkSubmitInfo marker{};marker.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    result=config_.dispatch.submit(queue_,1,&marker,fence_);
    submitted_=true;complete_=false;quiescent_=false;
    if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST)deviceLost_=true;return refuse(Reason::MarkerSubmit);}
    pending_=true;return advance(serial_);
}
Transaction::RetirementTicket Transaction::beforeRetirement(Retirement operation,VkCommandBuffer command,VkCommandPool pool) noexcept {
    Guard guard(*this);RetirementTicket ticket;if(!guard || !device_ || !generation_ || retired_)return ticket;
    switch(operation){case Retirement::ResetCommand:case Retirement::BeginCommand:case Retirement::FreeCommand:
        case Retirement::ResetPool:case Retirement::DestroyPool:break;default:return ticket;}
    const bool poolOperation=operation==Retirement::ResetPool || operation==Retirement::DestroyPool;
    if((poolOperation && (pool!=pool_ || !pool_)) || (!poolOperation && !matches(command)))return ticket;
    if(pendingSubmit_ || pendingRetirement_){refuse(Reason::Overlap);return ticket;}
    if(!advance(retireNonce_))return ticket;
    ticket.owner_=this;ticket.epoch_=epoch_;ticket.generation_=generation_;ticket.poolGeneration_=poolGeneration_;
    ticket.nonce_=retireNonce_;ticket.operation_=operation;pendingRetirement_=retireNonce_;return ticket;
}
bool Transaction::afterRetirement(const RetirementTicket& ticket,VkResult result) noexcept {
    Guard guard(*this);if(!guard)return false;
    if(ticket.owner_!=this || ticket.epoch_!=epoch_ || ticket.generation_!=generation_ || ticket.poolGeneration_!=poolGeneration_ ||
       !ticket.nonce_ || ticket.nonce_!=pendingRetirement_)return false;
    pendingRetirement_=0;if(result!=VK_SUCCESS)return false;
    // Only the saved OLD generation retires. Reset/Begin/Free/pool reuse cannot
    // be inferred by querying a replacement CB with the same numeric handle.
    retired_=true;recording_=false;executable_=false;
    if(!armed_){
        if(ticket.operation_==Retirement::FreeCommand || ticket.operation_==Retirement::DestroyPool)command_=VK_NULL_HANDLE;
        if(ticket.operation_==Retirement::DestroyPool)pool_=VK_NULL_HANDLE;
        if(ticket.operation_==Retirement::ResetPool || ticket.operation_==Retirement::DestroyPool)advance(poolGeneration_);
    }
    return true;
}
void Transaction::afterDeviceWaitIdle(VkResult result) noexcept {
    Guard guard(*this);if(!guard || !configured_)return;
    if(result==VK_SUCCESS)quiescent_=true;else if(result==VK_ERROR_DEVICE_LOST)deviceLost_=true;
}
bool Transaction::deviceDestroying(VkDevice device) noexcept {
    Guard guard(*this);if(!guard)return false;
    if(!device_ || device!=device_)return refuse(Reason::Identity);
    if(!quiescent_ && !deviceLost_)return false;
    // This is the actual original DestroyDevice ENTRY, not an ordinary idle
    // notification. Vulkan requires external synchronization of the device and
    // all its queues, so no valid future submission/recording may follow. The
    // separate actual idle/lost observation rules out pending GPU references.
    // Retire only this exact adopted generation and invalidate publication.
    retired_=true;recording_=false;executable_=false;
    pendingSubmit_=pendingRetirement_=0;refuse(Reason::DeviceDestroying);return true;
}
bool Transaction::publish(const SourceReadbackMetadata& readback,RecordView& out) noexcept {
    Guard guard(*this);out={};if(!guard || !configured_ || refusal()!=Reason::None)return false;
    if(!retired_)return false; // Pending is not a permanent refusal; callers may retry after retirement.
    if(!submitted_ || !fenceComplete())return false;
    if(readback.command!=command_ || readback.pool!=pool_ || readback.commandGeneration!=generation_ ||
       readback.poolGeneration!=poolGeneration_ || readback.execution!=serial_ || !same(readback.source,selectedSource_))return refuse(Reason::CohortMismatch);
    if(!clone_->hostCoherent()){
        VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=clone_->recordMemory();range.size=VK_WHOLE_SIZE;
        const auto result=config_.dispatch.invalidate(device_,1,&range);
        if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST)deviceLost_=true;return refuse(Reason::Fence);}
    }
    out.record=clone_->mappedRecord();out.bytes=frame_pixel_ssbo_owned::RecordBytes;out.source=readback.source;
    out.commandGeneration=generation_;out.execution=serial_;return out.record!=nullptr;
}
bool Transaction::release() noexcept {
    Guard guard(*this);if(!guard)return false;
    if(!configured_)return true;
    // A separately configured observer must never free another recording's
    // Clone reference. The cold owner is externally synchronized and exclusive;
    // this check also catches accidental sequential double-controller use.
    if(!armed_ && clone_->referenced())return false;
    // Error/refusal never loosens lifetime. A completed marker alone leaves the
    // reusable recording referencing owned objects. Device idle alone does too.
    if(armed_ && (!retired_ || (!quiescent_ && !deviceLost_)))return false;
    const auto result=clone_->release({!armed_ || retired_,!armed_ || quiescent_ || deviceLost_});
    if(result.status!=frame_pixel_ssbo_owned::Status::Ok)return false;
    config_.dispatch.destroyFence(device_,fence_,nullptr);fence_=VK_NULL_HANDLE;configured_=false;return true;
}
} // namespace frame_pixel_ssbo_transaction
