// Reuse owned structural CI builders without replaying their unrelated suites.
#pragma GCC diagnostic push
// The original main uses C++ implicit return0; this renamed helper is never run.
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main inherited_clone_fixture_not_run
#include "test_pixel_ssbo_clone_contract.cpp"
#undef main
#pragma GCC diagnostic pop
#include "../layer/native_presenter_pipeline.h"
#include "../layer/pixel_ssbo_profile_transform.h"

namespace creation {
namespace n=frame_native_presenter;
struct Driver {
    VkDevice device=object<VkDevice>(1);
    const VkAllocationCallbacks* allocator=nullptr;
    const VkGraphicsPipelineCreateInfo* original=nullptr;
    VkPipelineCache cache=object<VkPipelineCache>(0x91);
    VkShaderModule shader=object<VkShaderModule>(0x80);
    VkPipeline pipeline=object<VkPipeline>(0x81),old=object<VkPipeline>(0x60);
    VkResult shaderResult=VK_SUCCESS,pipelineResult=VK_SUCCESS;
    unsigned creates=0,destroyShaders=0,destroyPipelines=0;
    bool partial=false,emptyShader=false,emptyPipeline=false;
    std::vector<std::uint64_t> destroyed;
}* driver=nullptr;
void call(VkDevice device,const VkAllocationCallbacks* allocator){
    require(device==driver->device&&allocator==driver->allocator,"exact native device and allocator preserved");
}
VKAPI_ATTR VkResult VKAPI_CALL createShader(VkDevice device,const VkShaderModuleCreateInfo* ci,
        const VkAllocationCallbacks* allocator,VkShaderModule* out){
    call(device,allocator);++driver->creates;
    require(ci&&ci->sType==VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO&&!ci->pNext&&!ci->flags&&
        ci->codeSize==n::GuardedFragmentBytes,"only canonical guarded fragment enters new shader creation");
    if(driver->shaderResult!=VK_SUCCESS){*out=object<VkShaderModule>(0xdead);return driver->shaderResult;}
    *out=driver->emptyShader?VK_NULL_HANDLE:driver->shader;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyShader(VkDevice device,VkShaderModule value,const VkAllocationCallbacks* allocator){
    call(device,allocator);require(value==driver->shader,"destroy only successfully owned shader");++driver->destroyShaders;
}
VKAPI_ATTR VkResult VKAPI_CALL createPipeline(VkDevice device,VkPipelineCache cache,std::uint32_t count,
        const VkGraphicsPipelineCreateInfo* ci,const VkAllocationCallbacks* allocator,VkPipeline* out){
    call(device,allocator);++driver->creates;const auto& original=*driver->original;
    require(count==1&&cache==driver->cache,"same native cache and one bounded replacement");
    require(ci&&ci->sType==original.sType&&ci->pNext==original.pNext&&ci->flags==original.flags&&
        ci->stageCount==original.stageCount&&ci->pStages!=original.pStages&&
        ci->pVertexInputState==original.pVertexInputState&&ci->pInputAssemblyState==original.pInputAssemblyState&&
        ci->pTessellationState==original.pTessellationState&&ci->pViewportState==original.pViewportState&&
        ci->pRasterizationState==original.pRasterizationState&&ci->pMultisampleState==original.pMultisampleState&&
        ci->pDepthStencilState==original.pDepthStencilState&&ci->pColorBlendState==original.pColorBlendState&&
        ci->pDynamicState==original.pDynamicState&&ci->layout==original.layout&&ci->renderPass==original.renderPass&&
        ci->subpass==original.subpass&&ci->basePipelineHandle==original.basePipelineHandle&&
        ci->basePipelineIndex==original.basePipelineIndex,"all original CI members except stages retained");
    for(unsigned i=0;i<2;++i){const auto& a=original.pStages[i];const auto& b=ci->pStages[i];
        require(a.sType==b.sType&&a.pNext==b.pNext&&a.flags==b.flags&&a.stage==b.stage&&
            a.pName==b.pName&&a.pSpecializationInfo==b.pSpecializationInfo&&
            b.module==(a.stage==VK_SHADER_STAGE_FRAGMENT_BIT?driver->shader:a.module),
            "only fragment module changes, original stage order and pointers preserved");
    }
    *out=driver->pipelineResult==VK_SUCCESS?(driver->emptyPipeline?VK_NULL_HANDLE:driver->pipeline):
        (driver->partial?driver->pipeline:VK_NULL_HANDLE);
    return driver->pipelineResult;
}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice device,VkPipeline value,const VkAllocationCallbacks* allocator){
    call(device,allocator);require(value==driver->old||value==driver->pipeline,"only owned not-exposed original or partial replacement destroyed");
    ++driver->destroyPipelines;driver->destroyed.push_back(handle(value));
}
n::Dispatch dispatch(){return {createShader,destroyShader,createPipeline,destroyPipeline};}
std::vector<std::uint32_t> load(const char* path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"private bounded input opens");
    const auto size=f.tellg();require(size>0&&size<=65536&&size%4==0,"private input extent bounded");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size)/4);f.seekg(0);f.read(reinterpret_cast<char*>(words.data()),size);
    require(bool(f),"private input complete");return words;
}
void positive(const char* vertexPath,const char* fragmentPath,const char* candidatePath){
    auto vs=load(vertexPath),ps=load(fragmentPath),candidate=load(candidatePath);
    n::Fragment artifact;require(artifact.prepare(candidate.data(),candidate.size()*4),"canonical artifact positively admitted");
    // prepare owns the frozen words; changing caller memory cannot change code.
    candidate.assign(candidate.size(),0);
    require(artifact.words()[0]==0x07230203u,"candidate copy sealed before call");
    auto damaged=load(candidatePath);damaged.back()^=1;n::Fragment refused;
    require(!refused.prepare(damaged.data(),damaged.size()*4),"mutated executable rejected despite plausible interface");
    Fixture f;f.colorBlend.blendEnable=VK_FALSE;
    auto v=info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO),p=v;
    v.codeSize=vs.size()*4;v.pCode=vs.data();p.codeSize=ps.size()*4;p.pCode=ps.data();
    Recipe recipe;Refusal why;require(capture(f.inputs(v,p),recipe,why),"exact source recipe from owned native CI");
    VkAllocationCallbacks allocator{};
    auto run=[&](Driver& d,n::Reason expected,bool replaced,const Recipe& input,const n::Dispatch& calls){
        d.original=&f.graphics;d.allocator=&allocator;driver=&d;auto completed=d.old;
        const auto result=n::replaceCompletedPipeline(d.device,d.cache,VK_API_VERSION_1_1,VK_SUCCESS,f.graphics,&allocator,completed,input,artifact,calls);
        require(result.reason==expected&&result.replaced==replaced,"exact replacement outcome");
        require(completed==(replaced?d.pipeline:d.old),"failed replacement keeps original pipeline");
    };
    {Driver d;run(d,n::Reason::None,true,recipe,dispatch());
        require(d.creates==2&&d.destroyShaders==1&&d.destroyPipelines==1&&d.destroyed[0]==handle(d.old),"successful completion transfers normal app pipeline ownership");}
    {Driver d;d.shaderResult=VK_ERROR_OUT_OF_HOST_MEMORY;run(d,n::Reason::ShaderCreation,false,recipe,dispatch());
        require(d.creates==1&&!d.destroyShaders&&!d.destroyPipelines,"undefined failed shader output never destroyed");}
    for(bool partial:{false,true}){Driver d;d.pipelineResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;d.partial=partial;
        run(d,n::Reason::PipelineCreation,false,recipe,dispatch());
        require(d.creates==2&&d.destroyShaders==1&&d.destroyPipelines==unsigned(partial)&&
            (!partial||d.destroyed[0]==handle(d.pipeline)),"partial pipeline cleaned, original never destroyed on failure");}
    {Driver d;d.emptyShader=true;run(d,n::Reason::UnexpectedHandle,false,recipe,dispatch());
        require(d.creates==1&&!d.destroyShaders&&!d.destroyPipelines,"success-null shader leaves original untouched");}
    {Driver d;d.emptyPipeline=true;run(d,n::Reason::PipelineCreation,false,recipe,dispatch());
        require(d.creates==2&&d.destroyShaders==1&&!d.destroyPipelines,"success-null pipeline leaves original untouched");}
    {Driver d;auto wrong=recipe;wrong.provenance[1].generation=0;run(d,n::Reason::Identity,false,wrong,dispatch());require(!d.creates,"zero generation never calls driver");}
    {Driver d;auto wrong=recipe;wrong.provenance[5].value=0x61;run(d,n::Reason::Identity,false,wrong,dispatch());require(!d.creates,"different original pipeline never adopted");}
    {Driver d;auto wrong=recipe;wrong.vertex.words[7]^=1;run(d,n::Reason::Original,false,wrong,dispatch());require(!d.creates,"source pin rechecked");}
    {Driver d;auto wrong=recipe;wrong.set0.count=MaxBindings+1;run(d,n::Reason::Interface,false,wrong,dispatch());require(!d.creates,"count bound before read");}
    {Driver d;auto calls=dispatch();calls.createPipeline=nullptr;run(d,n::Reason::Dispatch,false,recipe,calls);require(!d.creates,"missing dispatch before side effects");}
    {Driver d;f.colorBlend.blendEnable=VK_TRUE;run(d,n::Reason::Graphics,false,recipe,dispatch());require(!d.creates,"blended native route remains original");f.colorBlend.blendEnable=VK_FALSE;}
    {Driver d;f.dynamics[0]=VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;run(d,n::Reason::Graphics,false,recipe,dispatch());require(!d.creates,"dynamic output state remains original");f.dynamics[0]=VK_DYNAMIC_STATE_LINE_WIDTH;}
    {Driver d;auto wrong=recipe;wrong.renderPass.attachments[0].format=VK_FORMAT_R8G8B8_UNORM;
        run(d,n::Reason::Graphics,false,wrong,dispatch());require(!d.creates,"RGB-only target cannot consume numerical alpha");}
    {Driver d;auto wrong=recipe;wrong.renderPass.colors[0].attachment=VK_ATTACHMENT_UNUSED;
        run(d,n::Reason::Graphics,false,wrong,dispatch());require(!d.creates,"unused color reference cannot consume alpha");}
    for(bool one:{false,true}){Driver d;
        (one?f.multisample.alphaToOneEnable:f.multisample.alphaToCoverageEnable)=VK_TRUE;
        run(d,n::Reason::Graphics,false,recipe,dispatch());require(!d.creates,"static alpha-affecting multisample states remain original");
        f.multisample.alphaToOneEnable=f.multisample.alphaToCoverageEnable=VK_FALSE;}
    {Driver d;VkBool32 disabled=VK_FALSE;
        auto colors=info<VkPipelineColorWriteCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT);
        colors.attachmentCount=1;colors.pColorWriteEnables=&disabled;f.blend.pNext=&colors;
        run(d,n::Reason::Graphics,false,recipe,dispatch());require(!d.creates,"static color-write disable remains original");f.blend.pNext=nullptr;}
    for(auto api:{VK_API_VERSION_1_0,VK_MAKE_API_VERSION(1,1,1,0)}){Driver d;driver=&d;auto completed=d.old;
        auto outcome=n::replaceCompletedPipeline(d.device,d.cache,api,VK_SUCCESS,f.graphics,&allocator,completed,recipe,artifact,dispatch());
        require(outcome.reason==n::Reason::ApiVersion&&!d.creates&&completed==d.old,"actual negotiated SPIR-V version floor required");}
    // Match the actual count1 native descriptor shape by reusing the separately
    // pinned original-interface attestation; no SSBO object is constructed/bound.
    {frame_pixel_ssbo_profile::Artifact mirror;
        require(frame_pixel_ssbo_profile::transform(recipe.fragment,false,mirror,why),"exact original count1 attestation");
        f.bindings[5].descriptorCount=1;auto inputs=f.inputs(v,p);inputs.probeInterface=mirror.profile;Recipe count1;
        require(capture(inputs,count1,why),"observed count1 layout captured without native expansion");
        Driver d;run(d,n::Reason::None,true,count1,dispatch());require(d.creates==2,"count1 original layouts reused");f.bindings[5].descriptorCount=3;}
    // Two distinct matched views each follow the same independent creation path.
    for(unsigned view=0;view<2;++view){Driver d;auto input=recipe;d.old=object<VkPipeline>(0x60+view);d.pipeline=object<VkPipeline>(0x81+view);input.provenance[5].value=0x60+view;
        run(d,n::Reason::None,true,input,dispatch());require(d.creates==2,"all views eligible, no first-draw selection");}
}
} // namespace creation
int main(int argc,char** argv){
    using namespace creation;
    if(argc==3&&std::string(argv[1])=="--scene-pin"){
        auto words=load(argv[2]);n::Fragment scene,synthetic;
        require(n::SceneFragmentBytes==31384&&scene.prepare(words.data(),words.size()*4,n::Profile::SceneContribution),"exact robust v3/v1 scene artifact admitted");
        require(!synthetic.prepare(words.data(),words.size()*4),"scene is not the historical straight-RGB synthetic artifact");
        words.back()^=1;n::Fragment corrupted;
        require(!corrupted.prepare(words.data(),words.size()*4,n::Profile::SceneContribution),"same-size dual mutation never self-authorizes");
        require(scene.ready()&&scene.words()[words.size()-1]!=(words.back()),"prepared scene owns sealed executable copy");
        require(!corrupted.prepare(words.data(),19224,n::Profile::SceneContribution),"legacy single-profile executable length is not the new dual capability");
        require(!corrupted.prepare(words.data(),31960,n::Profile::SceneContribution),"quarantined v2 executable length is not the robust capability");
        std::cout<<"{\"scope\":\"dual-scene-pin-only\",\"checks\":"<<checks<<",\"runtime_hooked\":false}\n";return 0;
    }
    require(argc==1||argc==4,"optional private VS PS guarded candidate paths");
    n::Fragment artifact;const std::array<std::uint32_t,5> fake{{0x07230203u,0x10100u,0,8,0}};
    require(!artifact.prepare(fake.data(),fake.size()*4)&&!artifact.ready(),"fake candidate cannot authorize creation");
    Recipe empty;Fixture f;Driver d;driver=&d;auto completed=d.old;
    auto result=n::replaceCompletedPipeline(d.device,d.cache,VK_API_VERSION_1_1,VK_SUCCESS,f.graphics,nullptr,completed,empty,artifact,dispatch());
    require(result.reason==n::Reason::Artifact&&!d.creates&&completed==d.old,"unprepared path has no side effects");
    if(argc==4)positive(argv[1],argv[2],argv[3]);
    std::cout<<"{\"scope\":\"inert-completion-scoped-creation\",\"checks\":"<<checks<<",\"private_positive\":"<<(argc==4?"true":"false")<<",\"runtime_hooked\":false}\n";
}
