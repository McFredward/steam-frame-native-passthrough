#pragma once

// Cold, finite CPU definition capture only. Caller-resolved object identities
// are diagnostics, not live handle leases. This header makes no Vulkan calls,
// owns no native objects and grants no receiver/runtime pipeline permission.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "../receiver/sha256.h"
#include "native_shader_contract.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace frame_pixel_ssbo_clone {
constexpr std::size_t MaxBindings=32, MaxArray=16, MaxPushRanges=8, MaxVertexBindings=16;
constexpr std::size_t MaxAttributes=32, MaxViewports=4, MaxAttachments=8, MaxDynamics=64, MaxDependencies=8;
constexpr std::size_t MaxChain=32, ShaderBytes=9460, MaxSubpasses=8;
enum class Reason : std::uint8_t {
    None, NativeCreationFailed, Type, Extension, Bounds, NullArray, Identity,
    FeatureAbsent, FeatureDisabled, FeatureValue, FeatureConflict, ChainCycle, DuplicateNode, LoaderFunction,
    ImmutableSampler, Descriptor, MissingImage9, Image9Extent, PushRange,
    ShaderProfile, ShaderPin, Stage, PipelineFlags, PipelineExtension, DynamicState,
    RenderPassMissing, RenderPassShape, AttachmentIndex, Scalar, OwnedObjectsMissing
};
struct Refusal {Reason reason=Reason::None;std::uint32_t index=UINT32_MAX,type=UINT32_MAX;};
inline const char* reasonName(Reason value) noexcept {
    switch(value){
#define NAME(e,s) case Reason::e:return s;
    NAME(None,"none") NAME(NativeCreationFailed,"native_creation_failed") NAME(Type,"type") NAME(Extension,"extension")
    NAME(Bounds,"bounds") NAME(NullArray,"null_array") NAME(Identity,"identity") NAME(FeatureAbsent,"feature_absent")
    NAME(FeatureDisabled,"feature_disabled") NAME(FeatureValue,"feature_value") NAME(FeatureConflict,"feature_conflict")
    NAME(ChainCycle,"chain_cycle") NAME(DuplicateNode,"duplicate_node") NAME(LoaderFunction,"loader_function")
    NAME(ImmutableSampler,"immutable_sampler") NAME(Descriptor,"descriptor") NAME(MissingImage9,"missing_image9")
    NAME(Image9Extent,"image9_extent") NAME(PushRange,"push_range") NAME(ShaderProfile,"shader_profile")
    NAME(ShaderPin,"shader_pin") NAME(Stage,"stage") NAME(PipelineFlags,"pipeline_flags")
    NAME(PipelineExtension,"pipeline_extension") NAME(DynamicState,"dynamic_state")
    NAME(RenderPassMissing,"renderpass_missing") NAME(RenderPassShape,"renderpass_shape")
    NAME(AttachmentIndex,"attachment_index") NAME(Scalar,"scalar") NAME(OwnedObjectsMissing,"owned_objects_missing")
#undef NAME
    }
    return "unknown_clone_refusal";
}
inline bool refuse(Refusal& out,Reason reason,std::uint32_t index=UINT32_MAX,
                   std::uint32_t type=UINT32_MAX) noexcept {
    if(out.reason==Reason::None)out={reason,index,type};
    return false;
}
inline bool boolean(VkBool32 value) noexcept{return value==VK_FALSE || value==VK_TRUE;}
inline bool stages(VkShaderStageFlags value) noexcept {
    return value && (value==VK_SHADER_STAGE_ALL || !(value&~VkShaderStageFlags(0x3f)));
}
inline bool digestMatches(const std::array<std::uint8_t,32>& digest,const char* hex) noexcept {
    constexpr char alphabet[]="0123456789abcdef";
    for(std::size_t i=0;i<digest.size();++i)if(alphabet[digest[i]>>4]!=hex[i*2] || alphabet[digest[i]&15]!=hex[i*2+1])return false;
    return true;
}
// Default preserves the exact original array3 interface. The only separate
// count1 profiles are deterministic original-invocation SSBO artifacts built
// by native_fragment_ssbo_profile.py: Set1, pixel(0,0), first-covered, or first
// eligible32x32grid invocation, exact original PS pin,
// two exhaustive B9 constant0 accesses and fresh owned interface types. The
// separate one-set mirror also attests BOTH native stages and unused binding8. A caller
// boolean/hash cannot attest that interface; admission hashes the actual bytes
// against this independently pinned transformer result. No native authority.
class ProbeInterface {
public:
    constexpr ProbeInterface() noexcept=default;
    bool specializedCount1() const noexcept{return kind_!=0;}
    bool firstCoveredInvocation() const noexcept{return kind_==2;}
    bool sparseGrid32() const noexcept{return kind_==3 || kind_==4;}
    bool compatibleSet0Mirror() const noexcept{return kind_==4;}
    std::uint32_t recordSet() const noexcept{return compatibleSet0Mirror()?0u:1u;}
    std::uint32_t recordBinding() const noexcept{return compatibleSet0Mirror()?MirrorRecordBinding:0u;}
    inline static constexpr std::array<std::uint32_t,7> MirrorBindings{{0,2,3,4,6,9,25}};
    static constexpr std::uint32_t MirrorRecordBinding=8;
    static constexpr std::size_t MirrorSparse32Bytes=9888;
    static constexpr const char* MirrorSparse32Sha256="ec8eaf5e1f0db2f4454320a5b613291e818d9f77b9c6219c683c1211e66333e1";
    static constexpr const char* OriginalVertexSha256="94abc0c24124e8b4a301e7f1df5a4c8c0e8b0eb50e4c6ea78f3521370a6897c5";
    static constexpr const char* OriginalFragmentSha256="f28dd5d22a570cbee64057da03d0418b80aee88a8eeed988b2f776e8d9a513f2";
    bool sameProfile(ProbeInterface other) const noexcept{return kind_==other.kind_;}
    static constexpr std::size_t Count1Bytes=9864;
    static constexpr const char* Count1Sha256="6d666c261114f93436e48c1f52793fe18afbce9552e07b53d4212654c4560020";
    static constexpr std::size_t FirstCoveredBytes=9616;
    static constexpr const char* FirstCoveredSha256="e76f8832c87229138e4c6b21bcab7b11e63d98bc016d42f5afb8dd7098c01bcd";
    static constexpr std::size_t Sparse32Bytes=9888;
    static constexpr const char* Sparse32Sha256="eb242f9252a51393e0794e12afed36a90de3e1059d45310d8b4c70d543019efd";
    static constexpr std::size_t MaximumBytes=Sparse32Bytes;
    static bool count1(const std::uint32_t* words,std::size_t bytes,ProbeInterface& out,Refusal& why) noexcept {
        out={};why={};
        if(!words || (bytes!=Count1Bytes && bytes!=FirstCoveredBytes && bytes!=Sparse32Bytes))return refuse(why,Reason::ShaderProfile);
        frame_receiver::Sha256 digest;digest.update(words,bytes);
        const auto hash=digest.finish();
        if(bytes==Count1Bytes && digestMatches(hash,Count1Sha256))out.kind_=1;
        else if(bytes==FirstCoveredBytes && digestMatches(hash,FirstCoveredSha256))out.kind_=2;
        else if(bytes==Sparse32Bytes && digestMatches(hash,Sparse32Sha256))out.kind_=3;
        else return refuse(why,Reason::ShaderPin);
        return true;
    }
    // A mirror profile cannot be minted from an artifact hash or caller flag
    // alone. Both exact native stage byte streams must independently prove the
    // complete descriptor interface, including absence of binding8. No original
    // instructions or binaries are distributed in this contract.
    static bool set0Mirror(const std::uint32_t* vertex,std::size_t vertexBytes,
                          const std::uint32_t* fragment,std::size_t fragmentBytes,
                          const std::uint32_t* probe,std::size_t probeBytes,
                          ProbeInterface& out,Refusal& why) noexcept {
        out={};why={};
        if(!pinned(vertex,vertexBytes,9460,OriginalVertexSha256)||
           !pinned(fragment,fragmentBytes,6620,OriginalFragmentSha256)||
           !pinned(probe,probeBytes,MirrorSparse32Bytes,MirrorSparse32Sha256))return refuse(why,Reason::ShaderPin);
        const NativeDescriptor vs[]={{75,2,2},{83,0,2},{142,3,2}};
        const NativeDescriptor ps[]={{89,4,2},{106,6,2},{289,9,0},{295,25,0}};
        if(!descriptors(vertex,vertexBytes,vs,3)||!descriptors(fragment,fragmentBytes,ps,4))return refuse(why,Reason::ShaderProfile);
        out.kind_=4;return true;
    }
    bool matches(const std::uint32_t* words,std::size_t bytes) const noexcept {
        if(!kind_)return true; // Historical array3 artifact transport contract.
        if(compatibleSet0Mirror())return pinned(words,bytes,MirrorSparse32Bytes,MirrorSparse32Sha256);
        ProbeInterface verified;Refusal why;return count1(words,bytes,verified,why) && verified.kind_==kind_;
    }
private:
    struct NativeDescriptor {std::uint32_t id,binding,storage;};
    static bool pinned(const std::uint32_t* words,std::size_t bytes,std::size_t expected,const char* pin) noexcept {
        if(!words || bytes!=expected)return false;
        frame_receiver::Sha256 hash;hash.update(words,bytes);return digestMatches(hash.finish(),pin);
    }
    // Separate bounded metadata walk. It never widens an executable grammar or
    // interprets another shader merely because its header looks plausible.
    static bool descriptors(const std::uint32_t* words,std::size_t bytes,const NativeDescriptor* expected,std::size_t entries) noexcept {
        if(words[0]!=0x07230203u || words[4] || !words[3] || words[3]>=65536 || entries>4)return false;
        std::array<unsigned,4> sets{},bindings{},variables{};
        for(std::size_t at=5;at<bytes/4;){
            const auto count=words[at]>>16,op=words[at]&65535u;if(!count||count>bytes/4-at)return false;
            if(op==73||op==74||op==75||op==332)return false; // no grouped/indirect descriptor annotations
            if(op==71&&count>=3&&(words[at+2]==33||words[at+2]==34)){
                if(count!=4)return false;
                std::size_t i=0;for(;i<entries&&expected[i].id!=words[at+1];++i){}if(i==entries)return false;
                if(words[at+2]==33){if(++bindings[i]!=1||words[at+3]!=expected[i].binding||words[at+3]==MirrorRecordBinding)return false;}
                else if(++sets[i]!=1||words[at+3])return false;
            }
            if(op==59){
                if(count<4)return false;
                std::size_t i=0;for(;i<entries&&expected[i].id!=words[at+2];++i){}
                if(i<entries){if(++variables[i]!=1||words[at+3]!=expected[i].storage)return false;}
                else if(words[at+3]==0||words[at+3]==2)return false;
            }
            at+=count;
        }
        for(std::size_t i=0;i<entries;++i)if(sets[i]!=1||bindings[i]!=1||variables[i]!=1)return false;
        return true;
    }
    std::uint8_t kind_=0;
};
template<class T> std::uint64_t handle(T value) noexcept {
    static_assert(sizeof(value)<=sizeof(std::uint64_t),"bounded public handle representation");
    std::uint64_t result=0;std::memcpy(&result,&value,sizeof(value));return result;
}
template<class T,std::size_t N> bool copyArray(const T* in,std::uint32_t count,std::array<T,N>& out,Refusal& why) noexcept {
    if(count>N)return refuse(why,Reason::Bounds);
    if(count && !in)return refuse(why,Reason::NullArray);
    for(std::uint32_t i=0;i<count;++i)out[i]=in[i];
    return true;
}
template<class T> bool ordinary(const T& in,VkStructureType type,Refusal& why) noexcept {
    if(in.sType!=type)return refuse(why,Reason::Type,UINT32_MAX,static_cast<std::uint32_t>(in.sType));
    return !in.pNext || refuse(why,Reason::Extension,UINT32_MAX,static_cast<std::uint32_t>(in.sType));
}
enum class FeatureSource : std::uint8_t {None,Legacy,Features2};
struct FeatureFacts {
    bool complete=false,observed=false,enabled=false;
    FeatureSource source=FeatureSource::None;
    VkBool32 raw=VK_FALSE;
    bool nativeAuthority=false;
};
// Public named scalar validation only; never scan struct padding or rewrite
// the original request. Non-storage feature declarations may be enabled or
// disabled independently. Their presence is not native route/lifetime proof.
inline bool featureValues(std::initializer_list<VkBool32> values,std::uint32_t index,
                          VkStructureType type,Refusal& why) noexcept {
    for(const auto value:values)if(!boolean(value))return refuse(why,Reason::FeatureValue,index,static_cast<std::uint32_t>(type));
    return true;
}
inline bool coreFeatureValues(const VkPhysicalDeviceFeatures& f,std::uint32_t index,
                              VkStructureType type,Refusal& why) noexcept {
    return featureValues({f.robustBufferAccess,f.fullDrawIndexUint32,f.imageCubeArray,f.independentBlend,f.geometryShader,
            f.tessellationShader,f.sampleRateShading,f.dualSrcBlend,f.logicOp,f.multiDrawIndirect,
            f.drawIndirectFirstInstance,f.depthClamp,f.depthBiasClamp,f.fillModeNonSolid,f.depthBounds,
            f.wideLines,f.largePoints,f.alphaToOne,f.multiViewport,f.samplerAnisotropy,f.textureCompressionETC2,
            f.textureCompressionASTC_LDR,f.textureCompressionBC,f.occlusionQueryPrecise,
            f.pipelineStatisticsQuery,f.vertexPipelineStoresAndAtomics,f.fragmentStoresAndAtomics,
            f.shaderTessellationAndGeometryPointSize,f.shaderImageGatherExtended,
            f.shaderStorageImageExtendedFormats,f.shaderStorageImageMultisample,
            f.shaderStorageImageReadWithoutFormat,f.shaderStorageImageWriteWithoutFormat,
            f.shaderUniformBufferArrayDynamicIndexing,f.shaderSampledImageArrayDynamicIndexing,
            f.shaderStorageBufferArrayDynamicIndexing,f.shaderStorageImageArrayDynamicIndexing,
            f.shaderClipDistance,f.shaderCullDistance,f.shaderFloat64,f.shaderInt64,f.shaderInt16,
            f.shaderResourceResidency,f.shaderResourceMinLod,f.sparseBinding,f.sparseResidencyBuffer,
            f.sparseResidencyImage2D,f.sparseResidencyImage3D,f.sparseResidency2Samples,
            f.sparseResidency4Samples,f.sparseResidency8Samples,f.sparseResidency16Samples,
            f.sparseResidencyAliased,f.variableMultisampleRate,f.inheritedQueries},index,type,why);
}
inline bool knownFeaturePayload(const VkBaseInStructure* node,std::uint32_t index,Refusal& why) noexcept {
    switch(node->sType){
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(node);
        return featureValues({f.storageBuffer16BitAccess,f.uniformAndStorageBuffer16BitAccess,f.storagePushConstant16,
            f.storageInputOutput16,f.multiview,f.multiviewGeometryShader,f.multiviewTessellationShader,
            f.variablePointersStorageBuffer,f.variablePointers,f.protectedMemory,f.samplerYcbcrConversion,
            f.shaderDrawParameters},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node);
        return featureValues({f.samplerMirrorClampToEdge,f.drawIndirectCount,f.storageBuffer8BitAccess,
            f.uniformAndStorageBuffer8BitAccess,f.storagePushConstant8,f.shaderBufferInt64Atomics,
            f.shaderSharedInt64Atomics,f.shaderFloat16,f.shaderInt8,f.descriptorIndexing,
            f.shaderInputAttachmentArrayDynamicIndexing,f.shaderUniformTexelBufferArrayDynamicIndexing,
            f.shaderStorageTexelBufferArrayDynamicIndexing,f.shaderUniformBufferArrayNonUniformIndexing,
            f.shaderSampledImageArrayNonUniformIndexing,f.shaderStorageBufferArrayNonUniformIndexing,
            f.shaderStorageImageArrayNonUniformIndexing,f.shaderInputAttachmentArrayNonUniformIndexing,
            f.shaderUniformTexelBufferArrayNonUniformIndexing,f.shaderStorageTexelBufferArrayNonUniformIndexing,
            f.descriptorBindingUniformBufferUpdateAfterBind,f.descriptorBindingSampledImageUpdateAfterBind,
            f.descriptorBindingStorageImageUpdateAfterBind,f.descriptorBindingStorageBufferUpdateAfterBind,
            f.descriptorBindingUniformTexelBufferUpdateAfterBind,
            f.descriptorBindingStorageTexelBufferUpdateAfterBind,f.descriptorBindingUpdateUnusedWhilePending,
            f.descriptorBindingPartiallyBound,f.descriptorBindingVariableDescriptorCount,
            f.runtimeDescriptorArray,f.samplerFilterMinmax,f.scalarBlockLayout,f.imagelessFramebuffer,
            f.uniformBufferStandardLayout,f.shaderSubgroupExtendedTypes,f.separateDepthStencilLayouts,
            f.hostQueryReset,f.timelineSemaphore,f.bufferDeviceAddress,f.bufferDeviceAddressCaptureReplay,
            f.bufferDeviceAddressMultiDevice,f.vulkanMemoryModel,f.vulkanMemoryModelDeviceScope,
            f.vulkanMemoryModelAvailabilityVisibilityChains,f.shaderOutputViewportIndex,f.shaderOutputLayer,
            f.subgroupBroadcastDynamicId},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(node);
        return featureValues({f.robustImageAccess,f.inlineUniformBlock,f.descriptorBindingInlineUniformBlockUpdateAfterBind,
            f.pipelineCreationCacheControl,f.privateData,f.shaderDemoteToHelperInvocation,
            f.shaderTerminateInvocation,f.subgroupSizeControl,f.computeFullSubgroups,f.synchronization2,
            f.textureCompressionASTC_HDR,f.shaderZeroInitializeWorkgroupMemory,f.dynamicRendering,
            f.shaderIntegerDotProduct,f.maintenance4},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node);
        return featureValues({f.timelineSemaphore},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(node);
        return featureValues({f.hostQueryReset},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceCustomBorderColorFeaturesEXT*>(node);
        return featureValues({f.customBorderColors,f.customBorderColorWithoutFormat},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*>(node);
        return featureValues({f.extendedDynamicState},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*>(node);
        return featureValues({f.extendedDynamicState3TessellationDomainOrigin,f.extendedDynamicState3DepthClampEnable,
            f.extendedDynamicState3PolygonMode,f.extendedDynamicState3RasterizationSamples,
            f.extendedDynamicState3SampleMask,f.extendedDynamicState3AlphaToCoverageEnable,
            f.extendedDynamicState3AlphaToOneEnable,f.extendedDynamicState3LogicOpEnable,
            f.extendedDynamicState3ColorBlendEnable,f.extendedDynamicState3ColorBlendEquation,
            f.extendedDynamicState3ColorWriteMask,f.extendedDynamicState3RasterizationStream,
            f.extendedDynamicState3ConservativeRasterizationMode,
            f.extendedDynamicState3ExtraPrimitiveOverestimationSize,f.extendedDynamicState3DepthClipEnable,
            f.extendedDynamicState3SampleLocationsEnable,f.extendedDynamicState3ColorBlendAdvanced,
            f.extendedDynamicState3ProvokingVertexMode,f.extendedDynamicState3LineRasterizationMode,
            f.extendedDynamicState3LineStippleEnable,f.extendedDynamicState3DepthClipNegativeOneToOne,
            f.extendedDynamicState3ViewportWScalingEnable,f.extendedDynamicState3ViewportSwizzle,
            f.extendedDynamicState3CoverageToColorEnable,f.extendedDynamicState3CoverageToColorLocation,
            f.extendedDynamicState3CoverageModulationMode,f.extendedDynamicState3CoverageModulationTableEnable,
            f.extendedDynamicState3CoverageModulationTable,f.extendedDynamicState3CoverageReductionMode,
            f.extendedDynamicState3RepresentativeFragmentTestEnable,
            f.extendedDynamicState3ShadingRateImageEnable},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
        const auto& f=*reinterpret_cast<const VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(node);
        return featureValues({f.samplerYcbcrConversion},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR: {
        const auto& f=*reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(node);
        return featureValues({f.presentId},index,node->sType,why);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR: {
        const auto& f=*reinterpret_cast<const VkPhysicalDevicePresentWaitFeaturesKHR*>(node);
        return featureValues({f.presentWait},index,node->sType,why);
    }
    default:return refuse(why,Reason::Extension,index,static_cast<std::uint32_t>(node->sType));
    }
}
inline bool enabledFeature(const VkDeviceCreateInfo& in,bool successfulCreate,
                           FeatureFacts& out,Refusal& why) noexcept {
    out={};why={};
    if(!successfulCreate)return refuse(why,Reason::NativeCreationFailed);
    if(in.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || in.flags)
        return refuse(why,Reason::Type,UINT32_MAX,static_cast<std::uint32_t>(in.sType));
    const auto observe=[&](VkBool32 value,FeatureSource source,std::uint32_t index=UINT32_MAX,
                           VkStructureType type=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO) noexcept {
        out.observed=true;out.raw=value;out.source=source;
        if(!boolean(value))return refuse(why,Reason::FeatureValue,index,static_cast<std::uint32_t>(type));
        out.enabled=value==VK_TRUE;return true;
    };
    if(in.pEnabledFeatures && (!observe(in.pEnabledFeatures->fragmentStoresAndAtomics,FeatureSource::Legacy) ||
       !coreFeatureValues(*in.pEnabledFeatures,UINT32_MAX,VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,why)))return false;
    std::array<const VkBaseInStructure*,MaxChain> seen{};std::uint32_t count=0,loaderFunctions=0;
    for(auto* node=static_cast<const VkBaseInStructure*>(in.pNext);node;node=node->pNext){
        if(count==seen.size())return refuse(why,Reason::Bounds,count);
        for(std::uint32_t i=0;i<count;++i){
            if(seen[i]==node)return refuse(why,Reason::ChainCycle,count);
            if(seen[i]->sType==node->sType && node->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
                return refuse(why,Reason::DuplicateNode,count,static_cast<std::uint32_t>(node->sType));
        }
        seen[count]=node;const auto index=count++;
        if(node->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO){
            const auto* loader=reinterpret_cast<const VkLayerDeviceCreateInfo*>(node);
            std::underlying_type_t<VkLayerFunction> function=0;
            std::memcpy(&function,&loader->function,sizeof(function));std::uint32_t bit=0;
            switch(function){
            case VK_LAYER_LINK_INFO:bit=1;break;case VK_LOADER_DATA_CALLBACK:bit=2;break;
            case VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK:bit=4;break;case VK_LOADER_FEATURES:bit=8;break;
            default:return refuse(why,Reason::LoaderFunction,index,static_cast<std::uint32_t>(node->sType));
            }
            if(loaderFunctions&bit)return refuse(why,Reason::DuplicateNode,index,static_cast<std::uint32_t>(node->sType));
            loaderFunctions|=bit;continue; // Never call/dereference a loader union callback.
        }
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2){
            if(in.pEnabledFeatures)return refuse(why,Reason::FeatureConflict,index,static_cast<std::uint32_t>(node->sType));
            const auto& features=reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features;
            if(!observe(features.fragmentStoresAndAtomics,FeatureSource::Features2,index,node->sType) ||
               !coreFeatureValues(features,index,node->sType,why))return false;
        }else if(!knownFeaturePayload(node,index,why))return false;
    }
    if(!out.observed)return refuse(why,Reason::FeatureAbsent);
    if(!out.enabled)return refuse(why,Reason::FeatureDisabled);
    out.complete=true;return true;
}

struct Set0Definition {
    std::uint32_t count=0;std::array<VkDescriptorSetLayoutBinding,MaxBindings> bindings{};bool complete=false;
    ProbeInterface probeInterface{};
    bool pendingCount1=false;
    bool view(VkDescriptorSetLayoutCreateInfo& out) const noexcept {
        out={};if(!complete || pendingCount1)return false;
        out.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        out.bindingCount=count;out.pBindings=bindings.data();return true;
    }
};
namespace detail {
inline bool captureSet0Shape(const VkDescriptorSetLayoutCreateInfo& in,Set0Definition& out,Refusal& why,
                            ProbeInterface profile,bool pending) noexcept {
    out={};why={};if(!ordinary(in,VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,why))return false;
    if(in.flags || in.bindingCount>MaxBindings)return refuse(why,Reason::Bounds);
    if(in.bindingCount && !in.pBindings)return refuse(why,Reason::NullArray);
    bool found9=false;
    for(std::uint32_t i=0;i<in.bindingCount;++i){
        const auto& binding=in.pBindings[i];
        if(binding.pImmutableSamplers)return refuse(why,Reason::ImmutableSampler,i);
        if(binding.descriptorCount>MaxArray || (binding.descriptorCount && !stages(binding.stageFlags)) ||
           binding.descriptorType<VK_DESCRIPTOR_TYPE_SAMPLER || binding.descriptorType>VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
            return refuse(why,Reason::Descriptor,i);
        for(std::uint32_t j=0;j<i;++j)if(in.pBindings[j].binding==binding.binding)return refuse(why,Reason::Descriptor,i);
        if(binding.binding==9){
            found9=true;
            if(binding.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || !(binding.stageFlags&VK_SHADER_STAGE_FRAGMENT_BIT))
                return refuse(why,Reason::Descriptor,i);
            // The exact original PS declares three descriptors. Actual Vulkan
            // validation requires that full extent even for its constant0
            // accesses. A separately validated specialized module needs its
            // own profile; native count1 must not silently pass this contract.
            if((profile.specializedCount1() || pending)?binding.descriptorCount!=1:binding.descriptorCount<3)
                return refuse(why,Reason::Image9Extent,i);
        }
        out.bindings[i]=binding; // Immutable pointers were rejected, never retained.
    }
    if(!found9)return refuse(why,Reason::MissingImage9);
    if(profile.compatibleSet0Mirror()){
        // The mirror copies only statically attested original descriptors.
        // Every dynamic binding still participates in bind-time offset/range
        // rules, even when neither pinned stage reads it. Do not bind an
        // undefined uncopied dynamic descriptor or infer that it is harmless.
        for(std::uint32_t i=0;i<in.bindingCount;++i){const auto& binding=in.pBindings[i];
            if(!binding.descriptorCount || (binding.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
               binding.descriptorType!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))continue;
            bool copied=false;for(const auto number:ProbeInterface::MirrorBindings)if(binding.binding==number)copied=true;
            if(!copied)return refuse(why,Reason::Descriptor,i);
        }
        for(std::size_t wanted=0;wanted<ProbeInterface::MirrorBindings.size()+1;++wanted){
            const auto number=wanted<ProbeInterface::MirrorBindings.size()?ProbeInterface::MirrorBindings[wanted]:ProbeInterface::MirrorRecordBinding;
            std::uint32_t i=0;for(;i<in.bindingCount&&in.pBindings[i].binding!=number;++i){}if(i==in.bindingCount)return refuse(why,Reason::Descriptor,i);
            const auto& binding=in.pBindings[i];const auto stage=wanted<3?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
            const bool type=wanted<5?(binding.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||binding.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC):
                wanted==5?binding.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:wanted==6?binding.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER:binding.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if(!type||binding.descriptorCount!=1||!(binding.stageFlags&stage))return refuse(why,Reason::Descriptor,i);
        }
    }
    out.count=in.bindingCount;out.probeInterface=profile;out.complete=!pending;out.pendingCount1=pending;return true;
}
} // namespace detail
inline bool captureSet0(const VkDescriptorSetLayoutCreateInfo& in,Set0Definition& out,Refusal& why,
                        ProbeInterface profile={}) noexcept {
    return detail::captureSet0Shape(in,out,why,profile,false);
}
// Cold observation only. No complete definition, usable Vulkan view or profile
// is produced until independently verified probe bytes authorize promotion.
inline bool capturePendingCount1Set0(const VkDescriptorSetLayoutCreateInfo& in,Set0Definition& out,Refusal& why) noexcept {
    return detail::captureSet0Shape(in,out,why,{},true);
}
inline bool promotePendingCount1Set0(Set0Definition& definition,ProbeInterface profile,Refusal& why) noexcept {
    why={};if(!definition.pendingCount1 || definition.complete || !profile.specializedCount1())return refuse(why,Reason::ShaderProfile);
    VkDescriptorSetLayoutCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount=definition.count;info.pBindings=definition.bindings.data();Set0Definition verified;
    if(!captureSet0(info,verified,why,profile))return false;
    definition=verified;return true;
}
struct LayoutDefinition {
    std::uint32_t count=0;std::array<VkPushConstantRange,MaxPushRanges> ranges{};bool complete=false;
};
inline bool captureLayout(const VkPipelineLayoutCreateInfo& in,VkDescriptorSetLayout resolvedSet0,
                          LayoutDefinition& out,Refusal& why) noexcept {
    out={};why={};if(!ordinary(in,VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,why))return false;
    if(in.flags || in.setLayoutCount!=1)return refuse(why,Reason::Bounds);
    if(!in.pSetLayouts || !resolvedSet0 || in.pSetLayouts[0]!=resolvedSet0)return refuse(why,Reason::Identity);
    if(!copyArray(in.pPushConstantRanges,in.pushConstantRangeCount,out.ranges,why))return false;
    VkShaderStageFlags used=0;
    for(std::uint32_t i=0;i<in.pushConstantRangeCount;++i){
        const auto& range=out.ranges[i];
        if(!stages(range.stageFlags) || (range.stageFlags&used) || !range.size || range.offset%4 || range.size%4 ||
           range.offset>4096 || range.size>4096-range.offset)return refuse(why,Reason::PushRange,i);
        used|=range.stageFlags;
    }
    out.count=in.pushConstantRangeCount;out.complete=true;return true;
}
// Full ordinary render-pass cohort, including subpasses which do not contain
// the observed draw. Flat inherited fields preserve the historical subpass0 API.
struct SubpassDefinition {
    std::uint32_t inputCount=0,colorCount=0,preserveCount=0;
    bool hasResolves=false,hasDepthReference=false,hasDepth=false;
    std::array<VkAttachmentReference,MaxAttachments> inputs{},colors{},resolves{};
    std::array<VkImageAspectFlags,MaxAttachments> inputAspects{};
    VkAttachmentReference depth{};std::array<std::uint32_t,MaxAttachments> preserves{};
};
// Call-duration views: recreate after copying/moving either view or definition.
struct RenderPassView {
    VkRenderPassCreateInfo info{};VkSubpassDescription subpass{};
    std::array<VkSubpassDescription,MaxSubpasses> subpasses{};
};
struct RenderPass2View {
    VkRenderPassCreateInfo2 info{};
    std::array<VkAttachmentDescription2,MaxAttachments> attachments{};
    std::array<VkSubpassDescription2,MaxSubpasses> subpasses{};
    std::array<std::array<VkAttachmentReference2,MaxAttachments>,MaxSubpasses> inputs{},colors{},resolves{};
    std::array<VkAttachmentReference2,MaxSubpasses> depths{};
    std::array<VkSubpassDependency2,MaxDependencies> dependencies{};
};
struct RenderPassDefinition:SubpassDefinition {
    std::uint32_t attachmentCount=0,dependencyCount=0,subpassCount=1;
    bool complete=false,route2=false,route2KHR=false;
    std::array<VkAttachmentDescription,MaxAttachments> attachments{};
    std::array<VkSubpassDependency,MaxDependencies> dependencies{};
    std::array<SubpassDefinition,MaxSubpasses-1> following{};
    const SubpassDefinition* subpass(std::uint32_t index) const noexcept {
        return index<subpassCount && index<MaxSubpasses?(index?&following[index-1]:static_cast<const SubpassDefinition*>(this)):nullptr;
    }
    SubpassDefinition* subpass(std::uint32_t index) noexcept {
        return index<subpassCount && index<MaxSubpasses?(index?&following[index-1]:static_cast<SubpassDefinition*>(this)):nullptr;
    }
    bool bounded() const noexcept {
        if(!subpassCount || subpassCount>MaxSubpasses || attachmentCount>MaxAttachments || dependencyCount>MaxDependencies)return false;
        for(std::uint32_t i=0;i<subpassCount;++i){const auto& sub=*subpass(i);
            if(sub.inputCount>MaxAttachments || sub.colorCount>MaxAttachments || sub.preserveCount>MaxAttachments)return false;
        }return true;
    }
    bool view(RenderPassView& out) const noexcept {
        out={};if(!complete || !bounded())return false;
        for(std::uint32_t n=0;n<subpassCount;++n){const auto& src=*subpass(n);auto& dst=out.subpasses[n];
            // RP2 input aspect selection has no corresponding legacy field.
            // Keep the older no-input lowering proof, never discard a used mask.
            if(route2)for(std::uint32_t i=0;i<src.inputCount;++i)
                if(src.inputs[i].attachment!=VK_ATTACHMENT_UNUSED)return false;
            dst.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;dst.inputAttachmentCount=src.inputCount;
            dst.pInputAttachments=src.inputCount?src.inputs.data():nullptr;dst.colorAttachmentCount=src.colorCount;
            dst.pColorAttachments=src.colorCount?src.colors.data():nullptr;dst.pResolveAttachments=src.hasResolves?src.resolves.data():nullptr;
            dst.pDepthStencilAttachment=src.hasDepthReference?&src.depth:nullptr;dst.preserveAttachmentCount=src.preserveCount;
            dst.pPreserveAttachments=src.preserveCount?src.preserves.data():nullptr;
        }
        out.subpass=out.subpasses[0];out.info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;out.info.attachmentCount=attachmentCount;
        out.info.pAttachments=attachmentCount?attachments.data():nullptr;out.info.subpassCount=subpassCount;
        out.info.pSubpasses=subpassCount==1?&out.subpass:out.subpasses.data();out.info.dependencyCount=dependencyCount;
        out.info.pDependencies=dependencyCount?dependencies.data():nullptr;return true;
    }
    bool view2(RenderPass2View& out) const noexcept {
        out={};if(!complete || !route2 || !bounded())return false;
        for(std::uint32_t i=0;i<attachmentCount;++i){const auto& a=attachments[i];
            out.attachments[i]={VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,nullptr,a.flags,a.format,a.samples,a.loadOp,a.storeOp,a.stencilLoadOp,a.stencilStoreOp,a.initialLayout,a.finalLayout};
        }
        for(std::uint32_t n=0;n<subpassCount;++n){const auto& src=*subpass(n);auto& dst=out.subpasses[n];dst.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
            dst.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;dst.inputAttachmentCount=src.inputCount;dst.colorAttachmentCount=src.colorCount;
            const auto copy=[](const VkAttachmentReference& ref,VkImageAspectFlags aspect) noexcept {return VkAttachmentReference2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,nullptr,ref.attachment,ref.layout,aspect};};
            for(std::uint32_t i=0;i<src.inputCount;++i)out.inputs[n][i]=copy(src.inputs[i],src.inputAspects[i]);
            for(std::uint32_t i=0;i<src.colorCount;++i){out.colors[n][i]=copy(src.colors[i],0);if(src.hasResolves)out.resolves[n][i]=copy(src.resolves[i],0);}
            if(src.hasDepthReference)out.depths[n]=copy(src.depth,0);
            dst.pInputAttachments=src.inputCount?out.inputs[n].data():nullptr;dst.pColorAttachments=src.colorCount?out.colors[n].data():nullptr;
            dst.pResolveAttachments=src.hasResolves?out.resolves[n].data():nullptr;dst.pDepthStencilAttachment=src.hasDepthReference?&out.depths[n]:nullptr;
            dst.preserveAttachmentCount=src.preserveCount;dst.pPreserveAttachments=src.preserveCount?src.preserves.data():nullptr;
        }
        for(std::uint32_t i=0;i<dependencyCount;++i){const auto& d=dependencies[i];
            out.dependencies[i]={VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,nullptr,d.srcSubpass,d.dstSubpass,d.srcStageMask,d.dstStageMask,d.srcAccessMask,d.dstAccessMask,d.dependencyFlags,0};
        }
        out.info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;out.info.attachmentCount=attachmentCount;
        out.info.pAttachments=attachmentCount?out.attachments.data():nullptr;out.info.subpassCount=subpassCount;out.info.pSubpasses=out.subpasses.data();
        out.info.dependencyCount=dependencyCount;out.info.pDependencies=dependencyCount?out.dependencies.data():nullptr;return true;
    }
};
inline bool sampleCount(VkSampleCountFlagBits value) noexcept {
    const auto v=static_cast<std::uint32_t>(value);return v && v<=64 && !(v&(v-1));
}
inline bool renderPassReferences(const RenderPassDefinition& pass,const SubpassDefinition& sub,Refusal& why,std::uint32_t index) noexcept {
    const auto ref=[&](VkAttachmentReference value) noexcept {return value.attachment==VK_ATTACHMENT_UNUSED || value.attachment<pass.attachmentCount;};
    for(std::uint32_t i=0;i<sub.inputCount;++i)if(!ref(sub.inputs[i]))return refuse(why,Reason::AttachmentIndex,index,i);
    for(std::uint32_t i=0;i<sub.colorCount;++i)if(!ref(sub.colors[i]) || (sub.hasResolves && !ref(sub.resolves[i])))return refuse(why,Reason::AttachmentIndex,index,i);
    if(sub.hasDepthReference && !ref(sub.depth))return refuse(why,Reason::AttachmentIndex,index);
    for(std::uint32_t i=0;i<sub.preserveCount;++i)if(sub.preserves[i]>=pass.attachmentCount)return refuse(why,Reason::AttachmentIndex,index,i);
    return true;
}
inline bool renderPassDependencies(const RenderPassDefinition& pass,Refusal& why) noexcept {
    for(std::uint32_t i=0;i<pass.dependencyCount;++i){const auto& d=pass.dependencies[i];
        if((d.srcSubpass!=VK_SUBPASS_EXTERNAL && d.srcSubpass>=pass.subpassCount) ||
           (d.dstSubpass!=VK_SUBPASS_EXTERNAL && d.dstSubpass>=pass.subpassCount) || (d.dependencyFlags&VK_DEPENDENCY_VIEW_LOCAL_BIT))return refuse(why,Reason::RenderPassShape,i);
    }return true;
}
inline bool captureRenderPass(const VkRenderPassCreateInfo* in,RenderPassDefinition& out,Refusal& why) noexcept {
    out={};why={};if(!in)return refuse(why,Reason::RenderPassMissing);
    if(!ordinary(*in,VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,why))return false;
    if(in->flags || !in->subpassCount)return refuse(why,Reason::RenderPassShape);
    if(in->subpassCount>MaxSubpasses)return refuse(why,Reason::Bounds);
    if(!in->pSubpasses)return refuse(why,Reason::NullArray);
    if(!copyArray(in->pAttachments,in->attachmentCount,out.attachments,why) || !copyArray(in->pDependencies,in->dependencyCount,out.dependencies,why))return false;
    out.attachmentCount=in->attachmentCount;out.dependencyCount=in->dependencyCount;out.subpassCount=in->subpassCount;
    for(std::uint32_t i=0;i<out.attachmentCount;++i)if((out.attachments[i].flags&~VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT) || !sampleCount(out.attachments[i].samples) || out.attachments[i].format==VK_FORMAT_UNDEFINED)return refuse(why,Reason::RenderPassShape,i);
    for(std::uint32_t n=0;n<out.subpassCount;++n){const auto& sub=in->pSubpasses[n];auto& dst=*out.subpass(n);
        if(sub.flags || sub.pipelineBindPoint!=VK_PIPELINE_BIND_POINT_GRAPHICS)return refuse(why,Reason::RenderPassShape,n);
        if(!copyArray(sub.pInputAttachments,sub.inputAttachmentCount,dst.inputs,why) || !copyArray(sub.pColorAttachments,sub.colorAttachmentCount,dst.colors,why) || !copyArray(sub.pPreserveAttachments,sub.preserveAttachmentCount,dst.preserves,why))return false;
        dst.inputCount=sub.inputAttachmentCount;dst.colorCount=sub.colorAttachmentCount;dst.preserveCount=sub.preserveAttachmentCount;
        // Resolve arrays are ignored when colorAttachmentCount is zero.
        dst.hasResolves=dst.colorCount && sub.pResolveAttachments;dst.hasDepthReference=sub.pDepthStencilAttachment!=nullptr;
        if(dst.hasResolves && !copyArray(sub.pResolveAttachments,dst.colorCount,dst.resolves,why))return false;
        if(dst.hasDepthReference){dst.depth=*sub.pDepthStencilAttachment;dst.hasDepth=dst.depth.attachment!=VK_ATTACHMENT_UNUSED;}
        if(!renderPassReferences(out,dst,why,n))return false;
    }
    if(!renderPassDependencies(out,why))return false;
    out.complete=true;return true;
}

// Preserve the original RP2 creation route, including input aspect selection.
// Non-input aspectMask is explicitly ignored by Vulkan; the owned view uses0:
// https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentReference2.html
inline bool captureRenderPass2(const VkRenderPassCreateInfo2* in,RenderPassDefinition& out,Refusal& why) noexcept {
    out={};why={};if(!in)return refuse(why,Reason::RenderPassMissing);
    if(!ordinary(*in,VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,why))return false;
    if(in->flags || !in->subpassCount || in->correlatedViewMaskCount)return refuse(why,Reason::RenderPassShape);
    if(in->subpassCount>MaxSubpasses || in->attachmentCount>MaxAttachments || in->dependencyCount>MaxDependencies)return refuse(why,Reason::Bounds);
    if(!in->pSubpasses || (in->attachmentCount && !in->pAttachments) || (in->dependencyCount && !in->pDependencies))return refuse(why,Reason::NullArray);
    out.attachmentCount=in->attachmentCount;out.dependencyCount=in->dependencyCount;out.subpassCount=in->subpassCount;
    for(std::uint32_t i=0;i<out.attachmentCount;++i){const auto& a=in->pAttachments[i];if(!ordinary(a,VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,why))return false;
        if((a.flags&~VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT) || !sampleCount(a.samples) || a.format==VK_FORMAT_UNDEFINED)return refuse(why,Reason::RenderPassShape,i);
        out.attachments[i]={a.flags,a.format,a.samples,a.loadOp,a.storeOp,a.stencilLoadOp,a.stencilStoreOp,a.initialLayout,a.finalLayout};
    }
    for(std::uint32_t n=0;n<out.subpassCount;++n){const auto& sub=in->pSubpasses[n];auto& dst=*out.subpass(n);
        if(!ordinary(sub,VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,why))return false;
        if(sub.flags || sub.viewMask || sub.pipelineBindPoint!=VK_PIPELINE_BIND_POINT_GRAPHICS)return refuse(why,Reason::RenderPassShape,n);
        if(sub.inputAttachmentCount>MaxAttachments || sub.colorAttachmentCount>MaxAttachments || sub.preserveAttachmentCount>MaxAttachments)return refuse(why,Reason::Bounds,n);
        if((sub.inputAttachmentCount && !sub.pInputAttachments) || (sub.colorAttachmentCount && !sub.pColorAttachments) || (sub.preserveAttachmentCount && !sub.pPreserveAttachments))return refuse(why,Reason::NullArray,n);
        dst.inputCount=sub.inputAttachmentCount;dst.colorCount=sub.colorAttachmentCount;dst.preserveCount=sub.preserveAttachmentCount;
        const auto ref=[&](const VkAttachmentReference2& src,VkAttachmentReference& target) noexcept {
            if(!ordinary(src,VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,why))return false;
            target={src.attachment,src.layout};return true;
        };
        for(std::uint32_t i=0;i<dst.inputCount;++i){if(!ref(sub.pInputAttachments[i],dst.inputs[i]))return false;dst.inputAspects[i]=sub.pInputAttachments[i].aspectMask;}
        dst.hasResolves=dst.colorCount && sub.pResolveAttachments;dst.hasDepthReference=sub.pDepthStencilAttachment!=nullptr;
        for(std::uint32_t i=0;i<dst.colorCount;++i){if(!ref(sub.pColorAttachments[i],dst.colors[i]) || (dst.hasResolves && !ref(sub.pResolveAttachments[i],dst.resolves[i])))return false;}
        if(dst.hasDepthReference){if(!ref(*sub.pDepthStencilAttachment,dst.depth))return false;dst.hasDepth=dst.depth.attachment!=VK_ATTACHMENT_UNUSED;}
        for(std::uint32_t i=0;i<dst.preserveCount;++i)dst.preserves[i]=sub.pPreserveAttachments[i];
        if(!renderPassReferences(out,dst,why,n))return false;
    }
    for(std::uint32_t i=0;i<out.dependencyCount;++i){const auto& d=in->pDependencies[i];if(!ordinary(d,VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,why))return false;
        if(d.viewOffset)return refuse(why,Reason::RenderPassShape,i);
        out.dependencies[i]={d.srcSubpass,d.dstSubpass,d.srcStageMask,d.dstStageMask,d.srcAccessMask,d.dstAccessMask,d.dependencyFlags};
    }
    if(!renderPassDependencies(out,why))return false;
    out.route2=true;out.complete=true;return true;
}

enum class ShaderRole : std::uint8_t {Vertex,Fragment};
struct ShaderDefinition {
    std::uint32_t bytes=0;std::array<std::uint32_t,ShaderBytes/4> words{};
    std::array<std::uint8_t,32> sha256{};bool complete=false;
};
inline bool captureShader(const VkShaderModuleCreateInfo& in,ShaderRole role,ShaderDefinition& out,Refusal& why) noexcept {
    out={};why={};if(!ordinary(in,VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,why))return false;
    if(role!=ShaderRole::Vertex && role!=ShaderRole::Fragment)return refuse(why,Reason::ShaderProfile);
    const std::size_t expected=role==ShaderRole::Vertex?9460:6620;
    if(in.flags || in.codeSize<20 || in.codeSize>65536 || in.codeSize%4 || !in.pCode || in.pCode[0]!=0x07230203u)
        return refuse(why,Reason::ShaderProfile);
    frame_receiver::Sha256 hash;hash.update(in.pCode,in.codeSize);out.sha256=hash.finish();
    const char* pin=role==ShaderRole::Vertex?"94abc0c24124e8b4a301e7f1df5a4c8c0e8b0eb50e4c6ea78f3521370a6897c5":
        "f28dd5d22a570cbee64057da03d0418b80aee88a8eeed988b2f776e8d9a513f2";
    if(in.codeSize==expected&&digestMatches(out.sha256,pin)){
        out.bytes=static_cast<std::uint32_t>(in.codeSize);std::memcpy(out.words.data(),in.pCode,in.codeSize);out.complete=true;return true;
    }
    namespace contract=stream_bridge_shader_contract;std::size_t used=0;
    const auto profile=role==ShaderRole::Vertex?contract::vertex():contract::fragment();
    if(!contract::normalize(in.pCode,in.codeSize,profile,out.words.data(),out.words.size(),used)||used*4!=expected)return refuse(why,Reason::ShaderProfile);
    frame_receiver::Sha256 canonical;canonical.update(out.words.data(),used*4);out.sha256=canonical.finish();
    if(!digestMatches(out.sha256,pin))return refuse(why,Reason::ShaderPin);
    out.bytes=static_cast<std::uint32_t>(used*4);out.complete=true;return true;
}

} // namespace frame_pixel_ssbo_clone
#include "pixel_graphics_definition.h"
namespace frame_pixel_ssbo_clone {

struct Identity {std::uint64_t device=0,value=0,generation=0;};
struct Inputs {
    std::uint64_t device=0;bool deviceCreateSucceeded=false;
    const VkDeviceCreateInfo* deviceCreate=nullptr;const VkDescriptorSetLayoutCreateInfo* set0=nullptr;
    const VkPipelineLayoutCreateInfo* layout=nullptr;const VkRenderPassCreateInfo* renderPass=nullptr;
    const VkShaderModuleCreateInfo* vertex=nullptr;const VkShaderModuleCreateInfo* fragment=nullptr;
    const VkGraphicsPipelineCreateInfo* graphics=nullptr;
    Identity set0Identity{},layoutIdentity{},renderPassIdentity{},vertexIdentity{},fragmentIdentity{},pipelineIdentity{};
    ProbeInterface probeInterface{};
};
// Call-duration view with a self-pointer; recreate after copying/moving it.
struct LayoutView {VkPipelineLayoutCreateInfo info{};std::array<VkDescriptorSetLayout,2> sets{};};
struct Recipe {
    FeatureFacts feature;Set0Definition set0;LayoutDefinition layout;RenderPassDefinition renderPass;
    ShaderDefinition vertex,fragment;GraphicsDefinition graphics;
    std::array<Identity,6> provenance{}; // Scalar diagnostic history, never build handles.
    bool definitionComplete=false,nativeCompatibilityProven=false,nativeLifetimeProven=false,runtimeActivationAuthorized=false;
    bool layoutView(VkDescriptorSetLayout ownedSet0,VkDescriptorSetLayout ownedSet1,LayoutView& out) const noexcept {
        out={};const bool mirror=set0.probeInterface.compatibleSet0Mirror();
        if(!definitionComplete || !ownedSet0 || (!mirror&&!ownedSet1))return false;
        out.sets={ownedSet0,mirror?VK_NULL_HANDLE:ownedSet1};out.info.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        out.info.setLayoutCount=mirror?1u:2u;out.info.pSetLayouts=out.sets.data();out.info.pushConstantRangeCount=layout.count;
        out.info.pPushConstantRanges=layout.count?layout.ranges.data():nullptr;return true;
    }
};
static_assert(sizeof(Recipe)<=65536,"bounded own cold recipe");
static_assert(std::is_trivially_copyable_v<Recipe>,"recipe has no borrowed native pointer ownership");
inline bool capture(const Inputs& in,Recipe& out,Refusal& why) noexcept {
    out={};why={};
    if(!in.deviceCreate || !in.set0 || !in.layout || !in.vertex || !in.fragment || !in.graphics)
        return refuse(why,Reason::NullArray);
    const std::array<Identity,6> identities{in.set0Identity,in.layoutIdentity,in.renderPassIdentity,
        in.vertexIdentity,in.fragmentIdentity,in.pipelineIdentity};
    if(!in.device)return refuse(why,Reason::Identity);
    for(std::size_t i=0;i<identities.size();++i)if(identities[i].device!=in.device || !identities[i].value || !identities[i].generation)
        return refuse(why,Reason::Identity,static_cast<std::uint32_t>(i));
    VkDescriptorSetLayout set0{};VkPipelineLayout layout{};VkRenderPass pass{};VkShaderModule vs{},ps{};
    std::memcpy(&set0,&in.set0Identity.value,sizeof(set0));std::memcpy(&layout,&in.layoutIdentity.value,sizeof(layout));
    std::memcpy(&pass,&in.renderPassIdentity.value,sizeof(pass));std::memcpy(&vs,&in.vertexIdentity.value,sizeof(vs));std::memcpy(&ps,&in.fragmentIdentity.value,sizeof(ps));
    if(!enabledFeature(*in.deviceCreate,in.deviceCreateSucceeded,out.feature,why) ||
       !captureSet0(*in.set0,out.set0,why,in.probeInterface) || !captureLayout(*in.layout,set0,out.layout,why) ||
       !captureRenderPass(in.renderPass,out.renderPass,why) || !captureShader(*in.vertex,ShaderRole::Vertex,out.vertex,why) ||
       !captureShader(*in.fragment,ShaderRole::Fragment,out.fragment,why) ||
       !captureGraphics(*in.graphics,layout,pass,vs,ps,out.renderPass,out.graphics,why,&out.feature))return false;
    out.provenance=identities;out.definitionComplete=true;return true;
}
} // namespace frame_pixel_ssbo_clone
