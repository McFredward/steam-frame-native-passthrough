#pragma once

// Optional cold create/destroy journal, NOT a draw-hook API. All pointer input
// is consumed synchronously before the intercepted create returns to its caller.
// Never call this class on a rendering callback: bounded copies, shader hashing
// and mutex scheduling can be expensive. No Vulkan calls, callbacks, allocation,
// I/O or logging occur under its mutex. Finite work is not an elapsed-time bound.
#include "pixel_ssbo_clone_contract.h"
#include "pixel_definition_facts.h"
#include <limits>
#include <mutex>

namespace frame_pixel_ssbo_journal {
namespace clone=frame_pixel_ssbo_clone;
// The retained startup archive has 107 distinct pipeline-layout handles before
// all six pinned target variants appear. It does not retain destruction or prove
// all 107 qualify. Reserve 2x that instance count, rounded to 256, for layouts
// and passes. Set handles use the same archived count rounded to 128. These
// are explicit finite instance budgets, not claimed native lifetime maxima.
// Unrelated layouts share facts rather than each copying full Set0 definitions.
// Unknown future pressure remains an explicit finite refusal, never eviction.
constexpr std::size_t SetCapacity=128, LayoutCapacity=256, PassCapacity=256;
// Each retained Set or Layout owns one reference. Therefore this many immutable
// semantic definitions cannot fill before the corresponding instance budgets.
constexpr std::size_t SetFactCapacity=SetCapacity+LayoutCapacity;
constexpr std::size_t ShaderCapacity=16, PipelineCapacity=16;
enum class PendingCount1 : std::uint8_t {Refuse,RetainFacts};
enum class ObjectKind : std::uint8_t {Device,Set,Layout,RenderPass,Shader,Pipeline};
enum class Reason : std::uint8_t {
    None,NativeCreationFailed,NotInitialized,AlreadyInitialized,DeviceIneligible,
    NullHandle,DuplicateHandle,Capacity,GenerationExhausted,UnknownSet,UnknownLayout,
    UnknownRenderPass,UnknownShader,Contract,NoReady,UnknownObject,StalePipeline,
    UnsupportedRoute,Count
};
inline const char* reasonName(Reason value) noexcept {
    switch(value){
#define NAME(e,s) case Reason::e:return s;
    NAME(None,"none") NAME(NativeCreationFailed,"native_creation_failed") NAME(NotInitialized,"not_initialized")
    NAME(AlreadyInitialized,"already_initialized") NAME(DeviceIneligible,"device_ineligible") NAME(NullHandle,"null_handle")
    NAME(DuplicateHandle,"duplicate_handle") NAME(Capacity,"capacity") NAME(GenerationExhausted,"generation_exhausted")
    NAME(UnknownSet,"unknown_set") NAME(UnknownLayout,"unknown_layout") NAME(UnknownRenderPass,"unknown_renderpass")
    NAME(UnknownShader,"unknown_shader") NAME(Contract,"contract") NAME(NoReady,"no_ready")
    NAME(UnknownObject,"unknown_object") NAME(StalePipeline,"stale_pipeline") NAME(UnsupportedRoute,"unsupported_route")
#undef NAME
    case Reason::Count:break;
    }
    return "unknown_journal_refusal";
}
struct Event {
    Reason reason=Reason::None;
    ObjectKind kind=ObjectKind::Device;
    std::uint64_t value=0,generation=0;
    clone::Refusal contract{};
};
static_assert(sizeof(Event)<=64,"fixed scalar refusal context");
struct PipelineToken {std::uint64_t device=0,value=0,generation=0,journalEpoch=0;};
struct Ready {
    clone::Recipe recipe{};
    PipelineToken pipeline{};
    // Own definitions only. A returned token is never a native lifetime lease.
    bool nativeAuthority=false;
};
struct Stats {
    std::uint64_t device=0,journalEpoch=0,created=0,destroyed=0,taken=0,cancelled=0;
    std::array<std::uint64_t,static_cast<std::size_t>(Reason::Count)> refusals{};
    std::uint64_t seenReasons=0;
    Event firstRefusal{};
    struct Context {Event first{},last{};std::uint64_t count=0;};
    std::array<Context,64> contexts{};
    std::uint32_t contextCount=0;
    std::uint64_t contextOverflow=0;
    std::array<std::uint32_t,6> live{},capacity{};
    std::uint32_t setFactCount=0,setFactCapacity=SetFactCapacity;
    clone::FeatureFacts feature{};
    clone::Refusal deviceRefusal{};
    bool initialized=false;
};

class Journal {
public:
    // An explicit nonzero seed supports deterministic wrap-boundary controls.
    // It is NOT a source-provided generation or a native authority assertion.
    explicit Journal(std::uint64_t firstGeneration=1,clone::ProbeInterface profile={},PendingCount1 pending=PendingCount1::Refuse):
        nextGeneration_(firstGeneration),probeInterface_(profile),pendingCount1_(pending){stats_.journalEpoch=acquireEpoch();}
    Journal(const Journal&)=delete;
    Journal& operator=(const Journal&)=delete;

    bool createDevice(VkDevice device,const VkDeviceCreateInfo& ci,VkResult result,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto value=clone::handle(device);clear(out,ObjectKind::Device,value);
        if(result!=VK_SUCCESS)return fail(out,Reason::NativeCreationFailed,ObjectKind::Device,value);
        if(stats_.initialized || retired_)return fail(out,Reason::AlreadyInitialized,ObjectKind::Device,value);
        if(!value)return fail(out,Reason::NullHandle,ObjectKind::Device,value);
        if(!stats_.journalEpoch)return fail(out,Reason::GenerationExhausted,ObjectKind::Device,value);
        // Even ineligible successful devices are initialized exactly once. The
        // fact/refusal remains inspectable; a failed original create initializes nothing.
        stats_.initialized=true;stats_.device=value;
        frame_pixel_definition_facts::device(facts_,ci,result);
        if(!clone::enabledFeature(ci,true,stats_.feature,stats_.deviceRefusal))
            return fail(out,Reason::DeviceIneligible,ObjectKind::Device,value,0,stats_.deviceRefusal);
        return true;
    }

    bool createSet(VkDescriptorSetLayout object,const VkDescriptorSetLayoutCreateInfo& ci,VkResult result,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);const auto value=clone::handle(object);
        if(!retired_)frame_pixel_definition_facts::set(facts_,ci,result);
        if(!beginCreate(sets_,value,result,ObjectKind::Set,out))return false;
        clone::Set0Definition definition;clone::Refusal why;
        // Original3 definitions keep their original profile. Only the explicit
        // count1 lane can use the separately verified specialized interface.
        if(!clone::captureSet0(ci,definition,why)){
            if(why.reason!=clone::Reason::Image9Extent)return fail(out,Reason::Contract,ObjectKind::Set,value,0,why);
            if(probeInterface_.specializedCount1()){
                if(!clone::captureSet0(ci,definition,why,probeInterface_))return fail(out,Reason::Contract,ObjectKind::Set,value,0,why);
            }else if(pendingCount1_!=PendingCount1::RetainFacts || !clone::capturePendingCount1Set0(ci,definition,why))
                return fail(out,Reason::Contract,ObjectKind::Set,value,0,why);
        }
        const auto index=retainSetFact(definition);
        if(index==SetFactCapacity)return fail(out,Reason::Capacity,ObjectKind::Set,value);
        if(insert(sets_,value,SetSnapshot{index},ObjectKind::Set,out))return true;
        releaseSetFact(index);return false;
    }
    // Successful native Set0/layout callbacks may precede the original PS
    // callback. Promote only bounded owned facts after the cold transformer
    // pins the actual probe. Identities/generations and creation totals remain
    // untouched; no callback replay, Vulkan call or native lifetime lease.
    bool authorizeProbeInterface(clone::ProbeInterface profile,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);clear(out,ObjectKind::Shader,0);
        if(!deviceEligible(out,ObjectKind::Shader,0))return false;
        if(!profile.specializedCount1())return fail(out,Reason::Contract,ObjectKind::Shader,0,0,{clone::Reason::ShaderProfile});
        if(probeInterface_.specializedCount1())return probeInterface_.sameProfile(profile) ||
            fail(out,Reason::Contract,ObjectKind::Shader,0,0,{clone::Reason::ShaderProfile});
        // Validate every owned pending semantic definition before changing any.
        // Temporary per-fact copies keep stack use independent of inventory size.
        clone::Refusal why;
        for(const auto& fact:setFacts_)if(fact.references && fact.definition.pendingCount1){
            auto verified=fact.definition;
            if(!clone::promotePendingCount1Set0(verified,profile,why))
                return fail(out,Reason::Contract,ObjectKind::Set,0,0,why);
        }
        for(auto& fact:setFacts_)if(fact.references && fact.definition.pendingCount1){
            // Same immutable input, same verified profile, no callbacks between
            // validation and promotion under this mutex: this cannot newly fail.
            clone::promotePendingCount1Set0(fact.definition,profile,why);
        }
        probeInterface_=profile;return true;
    }
    bool createLayout(VkPipelineLayout object,const VkPipelineLayoutCreateInfo& ci,VkResult result,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);const auto value=clone::handle(object);
        if(!beginCreate(layouts_,value,result,ObjectKind::Layout,out))return false;
        clone::Refusal shape;
        if(!clone::ordinary(ci,VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,shape))
            return fail(out,Reason::Contract,ObjectKind::Layout,value,0,shape);
        if(ci.flags)return fail(out,Reason::Contract,ObjectKind::Layout,value,0,{clone::Reason::Bounds});
        // Bounds precede reading an array supplied by the native callback.
        if(ci.setLayoutCount!=1 || !ci.pSetLayouts){
            clone::Refusal why;clone::LayoutDefinition ignored;
            clone::captureLayout(ci,VK_NULL_HANDLE,ignored,why);
            return fail(out,Reason::Contract,ObjectKind::Layout,value,0,why);
        }
        const auto* set=find(sets_,clone::handle(ci.pSetLayouts[0]));
        if(!set)return fail(out,Reason::UnknownSet,ObjectKind::Layout,value);
        LayoutSnapshot definition;clone::Refusal why;
        if(!clone::captureLayout(ci,ci.pSetLayouts[0],definition.layout,why))
            return fail(out,Reason::Contract,ObjectKind::Layout,value,0,why);
        definition.setFact=set->definition.setFact;definition.setIdentity=set->identity;
        if(!insert(layouts_,value,definition,ObjectKind::Layout,out))return false;
        ++setFacts_[definition.setFact].references;return true;
    }
    bool createRenderPass(VkRenderPass object,const VkRenderPassCreateInfo& ci,VkResult result,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);const auto value=clone::handle(object);
        if(!retired_)frame_pixel_definition_facts::renderPass(facts_,ci,result,value);
        if(!beginCreate(passes_,value,result,ObjectKind::RenderPass,out))return false;
        clone::RenderPassDefinition definition;clone::Refusal why;
        if(!clone::captureRenderPass(&ci,definition,why))return fail(out,Reason::Contract,ObjectKind::RenderPass,value,0,why);
        return insert(passes_,value,definition,ObjectKind::RenderPass,out);
    }
    bool createRenderPass2(VkRenderPass object,const VkRenderPassCreateInfo2& ci,VkResult result,Event* out=nullptr,bool route2KHR=false) {
        std::lock_guard<std::mutex> lock(mutex_);const auto value=clone::handle(object);
        if(!retired_)frame_pixel_definition_facts::renderPass2(facts_,ci,result,value);
        if(!beginCreate(passes_,value,result,ObjectKind::RenderPass,out))return false;
        clone::RenderPassDefinition definition;clone::Refusal why;
        if(!clone::captureRenderPass2(&ci,definition,why))return fail(out,Reason::Contract,ObjectKind::RenderPass,value,0,why);
        // The original caller's actual core/KHR command is the route evidence.
        // A nonnull promoted core PFN alone does not prove its version usable.
        definition.route2KHR=route2KHR;
        return insert(passes_,value,definition,ObjectKind::RenderPass,out);
    }
    bool createShader(VkShaderModule object,const VkShaderModuleCreateInfo& ci,VkResult result,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);const auto value=clone::handle(object);
        if(!beginCreate(shaders_,value,result,ObjectKind::Shader,out))return false;
        ShaderSnapshot definition;clone::Refusal why;
        // Recognize canonical shader semantics, including equivalent ID/debug
        // encodings. Exact old modules retain their original fast path.
        definition.role=ci.codeSize==6620?clone::ShaderRole::Fragment:clone::ShaderRole::Vertex;
        if(!clone::captureShader(ci,definition.role,definition.shader,why)){
            const auto first=why;
            definition.role=definition.role==clone::ShaderRole::Vertex?clone::ShaderRole::Fragment:clone::ShaderRole::Vertex;
            if(!clone::captureShader(ci,definition.role,definition.shader,why))return fail(out,Reason::Contract,ObjectKind::Shader,value,0,
                (ci.codeSize==9460||ci.codeSize==6620)?first:why);
        }
        return insert(shaders_,value,definition,ObjectKind::Shader,out);
    }
    bool createPipeline(VkPipeline object,const VkGraphicsPipelineCreateInfo& ci,VkResult result,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);const auto value=clone::handle(object);
        if(!retired_){
            namespace facts=frame_pixel_definition_facts;
            facts::graphics(facts_,ci,result,value);
            facts::GraphicsScope scope;
            // Resolve only owned facts from this exact original pass/subpass.
            // An unknown pass permits safe top-level observations, never reads
            // of ignored/possibly invalid depth or blend state pointers.
            if(ci.sType==VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO){
                const auto* pass=find(passes_,clone::handle(ci.renderPass));
                if(pass && pass->definition.complete){
                    const auto* subpass=pass->definition.subpass(ci.subpass);
                    if(subpass)scope={true,subpass->colorCount,subpass->hasDepth};
                }
            }
            facts::graphicsDetails(facts_,ci,result,value,scope);
        }
        if(!beginCreate(pipelines_,value,result,ObjectKind::Pipeline,out))return false;
        if(ci.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO)
            return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::Type});
        if(ci.pNext)return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::PipelineExtension});
        if(ci.flags || ci.basePipelineHandle)
            return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::PipelineFlags});
        const auto* layout=find(layouts_,clone::handle(ci.layout));
        if(!layout)return fail(out,Reason::UnknownLayout,ObjectKind::Pipeline,value);
        const auto& set0=setFacts_[layout->definition.setFact].definition;
        if(!set0.complete || set0.pendingCount1)
            return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::Image9Extent});
        const auto* pass=find(passes_,clone::handle(ci.renderPass));
        if(!pass)return fail(out,Reason::UnknownRenderPass,ObjectKind::Pipeline,value);
        if(ci.stageCount!=2 || !ci.pStages)
            return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::Stage});
        const Slot<ShaderSnapshot>* vertex=nullptr;const Slot<ShaderSnapshot>* fragment=nullptr;
        for(std::uint32_t i=0;i<ci.stageCount;++i){
            const auto* shader=find(shaders_,clone::handle(ci.pStages[i].module));
            if(!shader)return fail(out,Reason::UnknownShader,ObjectKind::Pipeline,value);
            if(ci.pStages[i].stage==VK_SHADER_STAGE_VERTEX_BIT && shader->definition.role==clone::ShaderRole::Vertex)vertex=shader;
            else if(ci.pStages[i].stage==VK_SHADER_STAGE_FRAGMENT_BIT && shader->definition.role==clone::ShaderRole::Fragment)fragment=shader;
            else return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::Stage,i});
        }
        if(!vertex || !fragment)return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,{clone::Reason::Stage});
        clone::Recipe recipe;clone::Refusal why;
        if(!clone::captureGraphics(ci,ci.layout,ci.renderPass,ci.pStages[ci.pStages[0].stage==VK_SHADER_STAGE_VERTEX_BIT?0:1].module,
                                  ci.pStages[ci.pStages[0].stage==VK_SHADER_STAGE_FRAGMENT_BIT?0:1].module,
                                  pass->definition,recipe.graphics,why,&stats_.feature))
            return fail(out,Reason::Contract,ObjectKind::Pipeline,value,0,why);
        recipe.feature=stats_.feature;recipe.set0=set0;recipe.layout=layout->definition.layout;
        recipe.renderPass=pass->definition;recipe.vertex=vertex->definition.shader;recipe.fragment=fragment->definition.shader;
        recipe.provenance={layout->definition.setIdentity,layout->identity,pass->identity,vertex->identity,fragment->identity,{}};
        auto* slot=empty(pipelines_); // beginCreate already checked the bounded inventory.
        if(!assignIdentity(slot->identity,value,ObjectKind::Pipeline,out))return false;
        recipe.provenance[5]=slot->identity;recipe.definitionComplete=true;
        slot->definition=recipe;slot->live=true;slot->ready=true;increment(stats_.created);
        clear(out,ObjectKind::Pipeline,value,slot->identity.generation);return true;
    }

    // Invoke at intercepted destroy ENTRY, BEFORE the downstream destroy. Doing
    // this after the driver returns permits a concurrent new create to reuse the
    // numeric handle before the old callback retires it. Never retire a newer
    // object from such an old post-destroy callback. No driver call is made here.
    bool destroySet(VkDescriptorSetLayout object,Event* out=nullptr){return destroy(sets_,clone::handle(object),ObjectKind::Set,out);}
    bool destroyLayout(VkPipelineLayout object,Event* out=nullptr){return destroy(layouts_,clone::handle(object),ObjectKind::Layout,out);}
    bool destroyRenderPass(VkRenderPass object,Event* out=nullptr){return destroy(passes_,clone::handle(object),ObjectKind::RenderPass,out);}
    bool destroyShader(VkShaderModule object,Event* out=nullptr){return destroy(shaders_,clone::handle(object),ObjectKind::Shader,out);}
    bool destroyPipeline(VkPipeline object,Event* out=nullptr){return destroy(pipelines_,clone::handle(object),ObjectKind::Pipeline,out);}
    void destroyDevice() {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto& slot:pipelines_)if(slot.live && slot.ready)increment(stats_.cancelled);
        sets_={};layouts_={};setFacts_={};passes_={};shaders_={};pipelines_={};retired_=true;
    }
    bool takeReady(Ready& out,Event* why=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);out={};clear(why,ObjectKind::Pipeline,0);
        if(!deviceEligible(why,ObjectKind::Pipeline,0))return false;
        Slot<clone::Recipe>* selected=nullptr;
        for(auto& slot:pipelines_)if(slot.live && slot.ready && (!selected || slot.identity.generation<selected->identity.generation))selected=&slot;
        if(!selected)return fail(why,Reason::NoReady,ObjectKind::Pipeline,0);
        out.recipe=selected->definition;out.pipeline={stats_.device,selected->identity.value,selected->identity.generation,stats_.journalEpoch};
        selected->ready=false;increment(stats_.taken);clear(why,ObjectKind::Pipeline,out.pipeline.value,out.pipeline.generation);return true;
    }
    // Select from the exact identity observed at a real original target draw.
    // This lookup neither consumes the reporting queue nor replays a create.
    // Already-reported live variants remain selectable; stale/reused handles
    // cannot silently select a different generation or the first ready recipe.
    bool lookupPipeline(clone::Identity identity,Ready& out,Event* why=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);out={};clear(why,ObjectKind::Pipeline,identity.value,identity.generation);
        if(!deviceEligible(why,ObjectKind::Pipeline,identity.value))return false;
        const auto* slot=find(pipelines_,identity.value);
        if(!identity.device || identity.device!=stats_.device || !identity.generation || !slot ||
           slot->identity.generation!=identity.generation)
            return fail(why,Reason::StalePipeline,ObjectKind::Pipeline,identity.value,identity.generation);
        out.recipe=slot->definition;out.pipeline={stats_.device,identity.value,identity.generation,stats_.journalEpoch};
        return true;
    }
    // Cold instantaneous lookup only, NOT a lease or permission to touch a native
    // pipeline after this method releases its mutex. Activation needs a separate
    // synchronized native lifetime/cancellation contract, absent from this class.
    bool current(const PipelineToken& token,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);clear(out,ObjectKind::Pipeline,token.value,token.generation);
        const auto* slot=find(pipelines_,token.value);
        if(retired_ || !token.device || token.device!=stats_.device || !token.journalEpoch || token.journalEpoch!=stats_.journalEpoch ||
           !token.generation || !slot || slot->identity.generation!=token.generation)
            return fail(out,Reason::StalePipeline,ObjectKind::Pipeline,token.value,token.generation);
        return true;
    }
    // Unsupported routes outside the explicit ordinary profiles only
    // records a refusal; it never alters the successful original Vulkan create.
    bool unsupported(ObjectKind kind,std::uint64_t value,std::uint32_t type,Event* out=nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fail(out,Reason::UnsupportedRoute,kind,value,0,{clone::Reason::Extension,UINT32_MAX,type});
    }
    Stats inspectStats() const {
        std::lock_guard<std::mutex> lock(mutex_);auto out=stats_;
        out.capacity={1,SetCapacity,LayoutCapacity,PassCapacity,ShaderCapacity,PipelineCapacity};
        out.live={stats_.initialized&&!retired_?1u:0u,liveCount(sets_),liveCount(layouts_),liveCount(passes_),liveCount(shaders_),liveCount(pipelines_)};
        for(const auto& fact:setFacts_)out.setFactCount+=fact.references?1u:0u;
        return out;
    }
    frame_pixel_definition_facts::Facts inspectFacts() const {std::lock_guard<std::mutex> lock(mutex_);return facts_;}

private:
    struct SetSnapshot {std::size_t setFact=SetFactCapacity;};
    struct SetFact {clone::Set0Definition definition{};std::uint32_t references=0;};
    struct LayoutSnapshot {std::size_t setFact=SetFactCapacity;clone::LayoutDefinition layout{};clone::Identity setIdentity{};};
    struct ShaderSnapshot {clone::ShaderDefinition shader{};clone::ShaderRole role=clone::ShaderRole::Vertex;};
    template<class T> struct Slot {clone::Identity identity{};T definition{};bool live=false,ready=false;};
    mutable std::mutex mutex_;
    std::uint64_t nextGeneration_=1;
    clone::ProbeInterface probeInterface_{};
    PendingCount1 pendingCount1_=PendingCount1::Refuse;
    bool retired_=false;
    Stats stats_{};
    frame_pixel_definition_facts::Facts facts_{};
    std::array<Slot<SetSnapshot>,SetCapacity> sets_{};
    std::array<SetFact,SetFactCapacity> setFacts_{};
    std::array<Slot<LayoutSnapshot>,LayoutCapacity> layouts_{};
    std::array<Slot<clone::RenderPassDefinition>,PassCapacity> passes_{};
    std::array<Slot<ShaderSnapshot>,ShaderCapacity> shaders_{};
    std::array<Slot<clone::Recipe>,PipelineCapacity> pipelines_{};
    static bool sameSet(const clone::Set0Definition& a,const clone::Set0Definition& b) noexcept {
        if(a.count!=b.count || a.complete!=b.complete || a.pendingCount1!=b.pendingCount1 || !a.probeInterface.sameProfile(b.probeInterface))return false;
        for(std::uint32_t i=0;i<a.count;++i){const auto& x=a.bindings[i];const auto& y=b.bindings[i];
            if(x.binding!=y.binding || x.descriptorType!=y.descriptorType || x.descriptorCount!=y.descriptorCount || x.stageFlags!=y.stageFlags)return false;
        }
        return true; // Immutable sampler pointers are rejected by capture.
    }
    std::size_t retainSetFact(const clone::Set0Definition& definition) noexcept {
        std::size_t unused=SetFactCapacity;
        for(std::size_t i=0;i<setFacts_.size();++i){auto& fact=setFacts_[i];
            if(fact.references && sameSet(fact.definition,definition)){++fact.references;return i;}
            if(!fact.references && unused==SetFactCapacity)unused=i;
        }
        if(unused!=SetFactCapacity)setFacts_[unused]={definition,1};
        return unused;
    }
    void releaseSetFact(std::size_t index) noexcept {
        if(index<setFacts_.size() && setFacts_[index].references && !--setFacts_[index].references)setFacts_[index]={};
    }
    static std::uint64_t acquireEpoch() {
        // Process-local diagnostic token namespace, not a native device epoch.
        // Constructor-only serialization never nests the instance mutex. On
        // exhaustion all future journals fail admission rather than reuse it.
        static std::mutex mutex;static std::uint64_t next=1;
        std::lock_guard<std::mutex> lock(mutex);
        if(!next)return 0;
        return next++;
    }
    static void increment(std::uint64_t& value) noexcept {if(value!=UINT64_MAX)++value;}
    static void clear(Event* out,ObjectKind kind,std::uint64_t value,std::uint64_t generation=0) noexcept {if(out)*out={Reason::None,kind,value,generation,{}};}
    bool fail(Event* out,Reason reason,ObjectKind kind,std::uint64_t value,std::uint64_t generation=0,clone::Refusal contract={}) noexcept {
        const Event event{reason,kind,value,generation,contract};if(out)*out=event;
        const auto index=static_cast<std::size_t>(reason);
        if(index<stats_.refusals.size()){increment(stats_.refusals[index]);stats_.seenReasons|=std::uint64_t(1)<<index;}
        if(stats_.firstRefusal.reason==Reason::None)stats_.firstRefusal=event;
        for(std::size_t i=0;i<stats_.contextCount;++i){
            auto& context=stats_.contexts[i];const auto& first=context.first;
            if(first.reason==reason && first.kind==kind && first.contract.reason==contract.reason &&
               first.contract.index==contract.index && first.contract.type==contract.type){
                context.last=event;increment(context.count);return false;
            }
        }
        if(stats_.contextCount<stats_.contexts.size())stats_.contexts[stats_.contextCount++]={event,event,1};
        else increment(stats_.contextOverflow);
        return false;
    }
    bool deviceEligible(Event* out,ObjectKind kind,std::uint64_t value) noexcept {
        if(!stats_.initialized)return fail(out,Reason::NotInitialized,kind,value);
        if(retired_ || !stats_.feature.complete || !stats_.feature.enabled)
            return fail(out,Reason::DeviceIneligible,kind,value,0,stats_.deviceRefusal);
        return true;
    }
    template<class T,std::size_t N> static Slot<T>* find(std::array<Slot<T>,N>& inventory,std::uint64_t value) noexcept {
        for(auto& slot:inventory)if(slot.live && slot.identity.value==value)return &slot;
        return nullptr;
    }
    template<class T,std::size_t N> static std::uint32_t liveCount(const std::array<Slot<T>,N>& inventory) noexcept {
        std::uint32_t count=0;for(const auto& slot:inventory)count+=slot.live?1u:0u;return count;
    }
    template<class T,std::size_t N> static Slot<T>* empty(std::array<Slot<T>,N>& inventory) noexcept {
        for(auto& slot:inventory)if(!slot.live)return &slot;
        return nullptr;
    }
    template<class T,std::size_t N> bool beginCreate(std::array<Slot<T>,N>& inventory,std::uint64_t value,VkResult result,ObjectKind kind,Event* out) noexcept {
        clear(out,kind,value);
        if(result!=VK_SUCCESS)return fail(out,Reason::NativeCreationFailed,kind,value);
        if(!deviceEligible(out,kind,value))return false;
        if(!value)return fail(out,Reason::NullHandle,kind,value);
        if(find(inventory,value))return fail(out,Reason::DuplicateHandle,kind,value);
        if(!empty(inventory))return fail(out,Reason::Capacity,kind,value);
        if(!nextGeneration_)return fail(out,Reason::GenerationExhausted,kind,value);
        return true;
    }
    bool assignIdentity(clone::Identity& out,std::uint64_t value,ObjectKind kind,Event* why) noexcept {
        if(!nextGeneration_)return fail(why,Reason::GenerationExhausted,kind,value);
        out={stats_.device,value,nextGeneration_++};return true; // Unsigned wrap becomes permanent refusal.
    }
    template<class T,std::size_t N> bool insert(std::array<Slot<T>,N>& inventory,std::uint64_t value,const T& definition,ObjectKind kind,Event* out) noexcept {
        auto* slot=empty(inventory);
        if(!assignIdentity(slot->identity,value,kind,out))return false;
        slot->definition=definition;slot->live=true;increment(stats_.created);clear(out,kind,value,slot->identity.generation);return true;
    }
    template<class T,std::size_t N> bool destroy(std::array<Slot<T>,N>& inventory,std::uint64_t value,ObjectKind kind,Event* out) {
        std::lock_guard<std::mutex> lock(mutex_);clear(out,kind,value);
        // Vulkan permits destroying a null handle; it is an unobserved no-op.
        if(!value)return true;
        auto* slot=find(inventory,value);
        if(!slot)return fail(out,Reason::UnknownObject,kind,value);
        const auto generation=slot->identity.generation;
        if(slot->ready)increment(stats_.cancelled);
        if constexpr(std::is_same_v<T,SetSnapshot> || std::is_same_v<T,LayoutSnapshot>)releaseSetFact(slot->definition.setFact);
        *slot={};increment(stats_.destroyed);clear(out,kind,value,generation);return true;
    }
};
static_assert(sizeof(Journal)<=2*1024*1024,"bounded per-device cold definition inventory");
static_assert(static_cast<unsigned>(Reason::Count)<64,"fixed deduplicated reason mask");
} // namespace frame_pixel_ssbo_journal
