// Inert current-R1 presenter semantics on owned Vulkan resources only.
// Allocation, original metadata and descriptors reuse the validated SSA fixture.
// The single-attachment adapter follows the existing packed-output fixture;
// neither source contains or redistributes a Valve shader instruction body.
#define main frame_original_fixture_unused_main
#include "../native-fragment-ssa-gpu/fixture.cpp"
#undef main
#include <memory>

namespace {
float floating(std::uint32_t v){float f;std::memcpy(&f,&v,4);return f;}
void near(float a,float b,const char* name){require(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=.000025f,std::string(name)+" differs");}
struct PresenterCase {
    Case native;
    Vec expected, colorU, maskU;
    float coverage, colorAperture, maskAperture, originalAperture;
    std::array<float,2> weights;
    std::array<float,3> inverseG;
    std::uint32_t kind;
    std::array<float,1024> texels;
};
static_assert(sizeof(PresenterCase)==4272,"fixed owned current-R1 case");
struct OutputGpu:Gpu {
    void singleDescriptors(unsigned count){
        // The explicitly specialized candidate uses count1; unchanged-original
        // comparison instances retain their declared count3 layout separately.
        vkDestroyPipelineLayout(device,pipelineLayout,nullptr);pipelineLayout={};
        vkDestroyDescriptorPool(device,descriptorPool,nullptr);descriptorPool={};
        vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);descriptorLayout={};
        VkDescriptorSetLayoutBinding bindings[4]{{4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {6,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {9,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {25,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo l{};l.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;l.bindingCount=4;l.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device,&l,nullptr,&descriptorLayout));
        VkDescriptorPoolSize sizes[3]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count},{VK_DESCRIPTOR_TYPE_SAMPLER,1}};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.poolSizeCount=3;pool.pPoolSizes=sizes;pool.maxSets=1;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&descriptorPool));
        VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;a.descriptorPool=descriptorPool;a.descriptorSetCount=1;a.pSetLayouts=&descriptorLayout;
        check(vkAllocateDescriptorSets(device,&a,&set));
        VkDescriptorBufferInfo buffersInfo[2]{{buffers[b4].value,0,272},{buffers[b6].value,0,2800}};
        VkDescriptorImageInfo imagesInfo[4]{{VK_NULL_HANDLE,images[source].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,images[source].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,images[source].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED}};
        VkWriteDescriptorSet writes[4]{};
        for(unsigned i=0;i<4;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=bindings[i].binding;w.descriptorCount=bindings[i].descriptorCount;w.descriptorType=bindings[i].descriptorType;
            if(i<2)w.pBufferInfo=&buffersInfo[i];else w.pImageInfo=&imagesInfo[i==2?0:3];}
        vkUpdateDescriptorSets(device,4,writes,0,nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,16};
        VkPipelineLayoutCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;info.setLayoutCount=1;info.pSetLayouts=&descriptorLayout;info.pushConstantRangeCount=1;info.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&info,nullptr,&pipelineLayout));
    }
    void singleResources(unsigned descriptors){
        resources();
        singleDescriptors(descriptors);
        vkDestroyFramebuffer(device,framebuffer,nullptr);framebuffer={};
        vkDestroyRenderPass(device,renderPass,nullptr);renderPass={};
        VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R32G32B32A32_SFLOAT;
        attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&reference;
        std::array<VkSubpassDependency,2> dep{};
        dep[0].srcSubpass=VK_SUBPASS_EXTERNAL;dep[0].dstSubpass=0;
        dep[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dep[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;dep[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass=0;dep[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dep[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;info.attachmentCount=1;info.pAttachments=&attachment;
        info.subpassCount=1;info.pSubpasses=&sub;info.dependencyCount=2;info.pDependencies=dep.data();
        check(vkCreateRenderPass(device,&info,nullptr,&renderPass));
        VkFramebufferCreateInfo fb{};fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fb.renderPass=renderPass;
        fb.attachmentCount=1;fb.pAttachments=&images[targets[0]].view;fb.width=fb.height=fb.layers=1;
        check(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer));
    }
    VkPipeline singlePipeline(VkShaderModule vertex,VkShaderModule fragment,const char* entry="PS_DISTORT",const Vec* source=nullptr,bool additive=false){
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto& s:stages)s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;stages[1].pName=entry;
        std::array<VkSpecializationMapEntry,4> entries{};VkSpecializationInfo specialization{};
        if(source){for(unsigned i=0;i<4;++i)entries[i]={i,i*4,4};specialization.mapEntryCount=4;specialization.pMapEntries=entries.data();specialization.dataSize=16;specialization.pData=source->data();stages[1].pSpecializationInfo=&specialization;}
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,1,1,0,1};VkRect2D scissor{{0,0},{1,1}};
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;rs.cullMode=VK_CULL_MODE_NONE;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
        if(source){blend.blendEnable=VK_TRUE;blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.colorBlendOp=VK_BLEND_OP_ADD;blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.alphaBlendOp=VK_BLEND_OP_ADD;
            if(additive){blend.srcColorBlendFactor=VK_BLEND_FACTOR_ONE;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
                blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;}}
        VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&blend;
        VkGraphicsPipelineCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;p.stageCount=2;p.pStages=stages;
        p.pVertexInputState=&vi;p.pInputAssemblyState=&ia;p.pViewportState=&vp;p.pRasterizationState=&rs;p.pMultisampleState=&ms;
        p.pColorBlendState=&cb;p.layout=pipelineLayout;p.renderPass=renderPass;
        VkPipeline result{};check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&p,nullptr,&result));return result;
    }
    void pipelinesSingle(const char* vertexPath,const char* oldPath,const char* outputPath,bool skipOriginal=false){
        VkShaderModule vertex{},old{},output{};auto release=[&]{for(auto m:{output,old,vertex})if(m)vkDestroyShaderModule(device,m,nullptr);};
        try{vertex=module(shader(vertexPath));if(!skipOriginal)old=module(shader(oldPath));output=module(shader(outputPath));if(!skipOriginal)original=singlePipeline(vertex,old);probe=singlePipeline(vertex,output);}catch(...){release();throw;}release();
    }
    void updateSource(const std::array<float,1024>& texels){
        std::memcpy(buffers[upload].mapped,texels.data(),4096);flush(upload);begin();
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=images[source].value;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
        vkCmdCopyBufferToImage(command,buffers[upload].value,images[source].value,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);finish();
    }
    void metadata(const Case& c){
        std::memset(buffers[b4].mapped,0,272);std::memset(buffers[b6].mapped,0,2800);
        auto word=[&](std::size_t b,unsigned at,std::uint32_t value){std::memcpy(static_cast<char*>(buffers[b].mapped)+at,&value,4);};
        for(unsigned i=0;i<4;++i)word(b4,160+i*4,bits(c.tint[i]));
        word(b4,256,c.eye);
        for(unsigned eye=0;eye<2;++eye){unsigned at=432+448*eye;
            for(unsigned i=0;i<16;++i)word(b6,at+i*4,bits(-.375f-float(eye)*.125f));
            for(unsigned i=0;i<4;++i){word(b6,at+i*16,bits(c.r0[i]+float(eye!=c.eye)*.0625f));word(b6,at+4+i*16,bits(c.r1[i]+float(eye!=c.eye)*.125f));}
            word(b6,at+3*4,c.flags^(eye!=c.eye?0x00400000u:0u));
            word(b6,at+11*4,bits(c.patch[0]+float(eye!=c.eye)*.1875f));word(b6,at+15*4,bits(c.patch[1]+float(eye!=c.eye)*.25f));}
        flush(b4);flush(b6);
    }
    void startPass(){
        begin();VkClearValue clear{};
        VkRenderPassBeginInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;rp.renderPass=renderPass;rp.framebuffer=framebuffer;
        rp.renderArea={{0,0},{1,1}};rp.clearValueCount=1;rp.pClearValues=&clear;
        vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
    }
    void raster(VkPipeline pipeline,const Vec& u){
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&set,0,nullptr);
        vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,16,u.data());vkCmdDraw(command,3,1,0,0);
    }
    std::array<std::uint32_t,4> readPass(){
        vkCmdEndRenderPass(command);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(command,images[targets[0]].value,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[readback].value,1,&copy);
        VkBufferMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buffers[readback].value;b.size=16;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);finish();
        if(!buffers[readback].coherent){VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=buffers[readback].memory;r.size=VK_WHOLE_SIZE;check(vkInvalidateMappedMemoryRanges(device,1,&r));}
        std::array<std::uint32_t,4> result{};std::memcpy(result.data(),buffers[readback].mapped,16);return result;
    }
    std::array<std::uint32_t,4> drawSingle(const Case& c,VkPipeline pipeline){metadata(c);startPass();raster(pipeline,c.u);return readPass();}
    std::array<Vec,4> transparentSources(const char* vertexPath,const char* fragmentPath,bool premultiplied){
        VkShaderModule vertex{},fragment{};VkPipeline back{},front{},add{};
        auto release=[&]{for(auto p:{add,front,back})if(p)vkDestroyPipeline(device,p,nullptr);for(auto m:{fragment,vertex})if(m)vkDestroyShaderModule(device,m,nullptr);};
        std::array<Vec,4> result{};
        try{vertex=module(shader(vertexPath));fragment=module(shader(fragmentPath));
            const Vec behind{.8f,.2f,.1f,.25f},glass{.2f,.7f,.3f,.5f},emission{.12f,.18f,.08f,0.f};
            back=singlePipeline(vertex,fragment,"main",&behind);front=singlePipeline(vertex,fragment,"main",&glass);
            if(premultiplied)add=singlePipeline(vertex,fragment,"main",&emission,true);
            for(unsigned kind=0;kind<(premultiplied?4u:1u);++kind){
                startPass();
                if(kind==0||kind==2){raster(back,{0,0,0,0});raster(front,{0,0,0,0});}
                if(kind!=0)raster(add,{0,0,0,0});
                if(kind==3)raster(front,{0,0,0,0});
                const auto observed=readPass();
                for(unsigned i=0;i<3;++i){
                    const float overlap=glass[i]*glass[3]+behind[i]*behind[3]*(1-glass[3]);
                    const float expected=kind==0?overlap:kind==1?emission[i]:kind==2?overlap+emission[i]:glass[i]*glass[3]+emission[i]*(1-glass[3]);
                    result[kind][i]=floating(observed[i]);near(result[kind][i],expected,"true GPU source blend color");
                }
                result[kind][3]=floating(observed[3]);
                near(result[kind][3],kind==0||kind==2?.625f:kind==1?0.f:.5f,"true GPU source coverage");
                if(kind==1)require(result[kind][0]>0&&result[kind][1]>0&&result[kind][2]>0,"actual alpha0 additive emission retained");
            }
        }catch(...){release();throw;}release();return result;
    }
};
std::vector<PresenterCase> readPresenterCases(const char* path){
    std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"explicit current-R1 cases");
    const auto bytes=file.tellg();require(bytes>=12&&bytes<=12+256*4272,"bounded current-R1 case file");
    file.seekg(0);char magic[8]{};std::uint32_t count{};
    require(bool(file.read(magic,8))&&std::memcmp(magic,"R1PRES1",8)==0,"owned current-R1 case schema");
    require(bool(file.read(reinterpret_cast<char*>(&count),4))&&count>0&&count<=256,"bounded current-R1 cases");
    require(bytes==std::streamoff(12+count*4272),"complete current-R1 record range");
    std::vector<PresenterCase> result(count);require(bool(file.read(reinterpret_cast<char*>(result.data()),count*4272)),"complete current-R1 cases");
    for(const auto& c:result){
        require(c.native.eye<2&&c.kind<3,"owned selector and source kind");
        for(const auto* v:{&c.native.u,&c.native.r0,&c.native.r1,&c.native.patch,&c.native.tint,&c.expected,&c.colorU,&c.maskU})
            for(float f:*v)require(std::isfinite(f),"finite owned input");
        for(float f:{c.coverage,c.colorAperture,c.maskAperture,c.originalAperture,c.weights[0],c.weights[1],c.inverseG[0],c.inverseG[1],c.inverseG[2]})
            require(std::isfinite(f),"finite owned oracle coefficient");
        require(c.coverage>=0&&c.coverage<=1,"explicit linear source coverage");
        for(float f:c.texels)require(std::isfinite(f)&&f>=0&&f<=1,"normalized synthetic source component");
    }
    return result;
}
}
#ifndef SFNP_R1_FIXTURE_NO_MAIN
int main(int argc,char** argv){
    try{
        require(argc==7,"usage: r1-presenter-fixture VERTEX ORIGINAL PRESENTER CASES enabled|disabled 1|3");
        const bool enabled=std::string(argv[5])=="enabled";
        require(enabled||std::string(argv[5])=="disabled","explicit enabled mode");
        const unsigned descriptors=std::string(argv[6])=="1"?1:3;
        require(std::string(argv[6])=="1"||std::string(argv[6])=="3","explicit original descriptor count");
        require(enabled||descriptors==3,"OFF is exact original array3");
        const auto input=readPresenterCases(argv[4]);
        OutputGpu gpu;gpu.init();require(gpu.validationEnabled,"Khronos validation active");
        gpu.singleResources(descriptors);gpu.pipelinesSingle(argv[1],argv[2],argv[3],descriptors==1);
        require(validationErrors.load()==0,"pipeline Vulkan validation errors");
        std::unique_ptr<OutputGpu> separate;
        if(descriptors==1){separate=std::make_unique<OutputGpu>();separate->init();require(separate->validationEnabled,"original oracle validation active");
            separate->singleResources(3);separate->pipelinesSingle(argv[1],argv[2],argv[2]);}
        OutputGpu& reference=separate?*separate:gpu;
        unsigned complete=0,colorOracles=0,maskOracles=0,partial=0,zero=0,full=0,unequal=0,partialInner=0,partialAperture=0;
        unsigned selectors=0,flags=0;float maximumError=0;
        for(const auto& c:input){
            gpu.updateSource(c.texels);if(separate)reference.updateSource(c.texels);
            const auto original=reference.drawSingle(c.native,reference.original);
            const auto output=gpu.drawSingle(c.native,gpu.probe);
            const auto restored=reference.drawSingle(c.native,reference.original);
            require(original==restored,"exact original/off recovery");require(original[3]==bits(1.f),"original remains opaque");
            if(enabled){
                for(unsigned i=0;i<4;++i){
                    const float observed=floating(output[i]);
                    near(observed,c.expected[i],i==3?"current-R1 explicit alpha":"current-R1 premultiplied color");
                    maximumError=std::max(maximumError,std::abs(observed-c.expected[i]));
                }
                Case color=c.native;color.u=c.colorU;
                Case mask=c.native;mask.u=c.maskU;mask.tint={1,1,1,1};
                const auto nativeColor=reference.drawSingle(color,reference.original);
                const auto nativeMask=reference.drawSingle(mask,reference.original);
                float rawG=0;
                if(c.maskAperture>.0001f){
                    for(unsigned i=0;i<3;++i)rawG+=floating(nativeMask[i])*c.inverseG[i];
                    rawG/=c.maskAperture;
                    near(rawG,c.coverage,"independent original inverse-decoder raw G");
                    near(floating(output[3]),rawG*c.originalAperture,"independent original raw-G alpha oracle");++maskOracles;
                }
                if(c.colorAperture>.0001f&&c.maskAperture>.0001f){
                    for(unsigned i=0;i<3;++i)near(floating(output[i]),floating(nativeColor[i])/c.colorAperture*c.originalAperture*rawG,
                        "independent original color and mask GPU oracle");++colorOracles;
                }
            }else require(output==original,"disabled exact original GPU output");
            partial+=c.coverage>0&&c.coverage<1;zero+=c.coverage==0;full+=c.coverage==1;
            unequal+=std::abs(c.weights[0]-c.weights[1])>.01f;
            partialInner+=(c.weights[0]>.01f&&c.weights[0]<.99f)||(c.weights[1]>.01f&&c.weights[1]<.99f);
            partialAperture+=c.originalAperture>.01f&&c.originalAperture<.99f;
            selectors|=1u<<c.native.eye;flags|=1u<<((c.native.flags>>17)&3);++complete;
        }
        require(zero&&partial&&full&&unequal&&partialInner&&partialAperture,"coverage weights and aperture exercised");
        require(selectors==3&&flags==15,"both metadata selectors and all decoder/reconstruction branches exercised");
        require(!enabled||(colorOracles>=8&&maskOracles>=8),"independent unchanged-original GPU oracles");
        require(validationErrors.load()==0,"Vulkan validation errors");
        std::cout<<"{\"schema\":\"frame-native-r1-presenter-gpu-v1\",\"cases\":"<<complete<<",\"checks\":"<<checks
            <<",\"original_color_gpu_oracles\":"<<colorOracles<<",\"original_mask_gpu_oracles\":"<<maskOracles
            <<",\"zero_coverage_cases\":"<<zero<<",\"partial_coverage_cases\":"<<partial<<",\"full_coverage_cases\":"<<full
            <<",\"unequal_weight_cases\":"<<unequal<<",\"partial_inner_weight_cases\":"<<partialInner<<",\"partial_aperture_cases\":"<<partialAperture
            <<",\"original_off_recovery_bit_identical\":true,\"max_absolute_error\":"<<maximumError
            <<",\"validation_enabled\":true,\"validation_errors\":"<<validationErrors.load()<<",\"color_attachment_count\":1"
            <<",\"sampled_image_descriptor_count\":"<<descriptors<<",\"device_name\":\""<<gpu.properties.deviceName<<"\",\"passthrough_authorized\":false}\n";return 0;
    }catch(const std::exception& e){std::cerr<<"current-R1 presenter fixture failed: "<<e.what()<<"; checks="<<checks<<'\n';return 1;}
}

#endif
