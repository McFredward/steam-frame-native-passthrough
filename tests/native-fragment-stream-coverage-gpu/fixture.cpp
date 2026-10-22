// Executes the locally transformed native fragment with an independent R8 mask.
// Native RGB has an independent unchanged-original GPU oracle in every case.
#define SFNP_R1_FIXTURE_NO_MAIN
#include "../native-fragment-r1-presenter-gpu/fixture.cpp"
namespace {
struct CoverageGpu:OutputGpu {
    VkDescriptorSetLayout maskLayout{},controlLayout{};VkDescriptorPool maskPool{};VkDescriptorSet maskSet{},controlSet{};
    std::size_t control=0;
    std::size_t maskImage=0,maskUpload=0;
    ~CoverageGpu(){if(maskPool)vkDestroyDescriptorPool(device,maskPool,nullptr);if(maskLayout)vkDestroyDescriptorSetLayout(device,maskLayout,nullptr);if(controlLayout)vkDestroyDescriptorSetLayout(device,controlLayout,nullptr);}
    void prepareMask(){
        maskImage=image(VK_FORMAT_R8_UNORM,16,16,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        maskUpload=buffer(256,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};l.bindingCount=1;l.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&l,nullptr,&maskLayout));
        binding.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        check(vkCreateDescriptorSetLayout(device,&l,nullptr,&controlLayout));
        control=buffer(16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        vkDestroyPipelineLayout(device,pipelineLayout,nullptr);pipelineLayout={};
        VkDescriptorSetLayout layouts[3]{descriptorLayout,maskLayout,controlLayout};
        VkPipelineLayoutCreateInfo p{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};p.setLayoutCount=3;p.pSetLayouts=layouts;
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT,0,16};p.pushConstantRangeCount=1;p.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device,&p,nullptr,&pipelineLayout));
        VkDescriptorPoolSize size[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=2;pool.pPoolSizes=size;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&maskPool));
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=maskPool;a.descriptorSetCount=1;a.pSetLayouts=&maskLayout;
        check(vkAllocateDescriptorSets(device,&a,&maskSet));
        a.pSetLayouts=&controlLayout;check(vkAllocateDescriptorSets(device,&a,&controlSet));
        VkDescriptorImageInfo info{sampler,images[maskImage].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=maskSet;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info;
        vkUpdateDescriptorSets(device,1,&w,0,nullptr);
        VkDescriptorBufferInfo controlInfo{buffers[control].value,0,16};
        w.dstSet=controlSet;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pImageInfo=nullptr;w.pBufferInfo=&controlInfo;
        vkUpdateDescriptorSets(device,1,&w,0,nullptr);
        auto* pixels=static_cast<unsigned char*>(buffers[maskUpload].mapped);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)pixels[y*16+x]=std::array<unsigned char,4>{{0,64,127,255}}[x/4];
        flush(maskUpload);begin();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[maskImage].value;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
        vkCmdCopyBufferToImage(command,buffers[maskUpload].value,images[maskImage].value,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);finish();
    }
    std::array<std::uint32_t,4> drawMasked(const Case& c,float enabled=1,float recovery=0,bool bindMask=true){const std::array<float,4> values{{enabled,recovery,0,0}};std::memcpy(buffers[control].mapped,values.data(),sizeof values);flush(control);metadata(c);startPass();if(bindMask)vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,&maskSet,0,nullptr);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,2,1,&controlSet,0,nullptr);raster(probe,c.u);return readPass();}
};
}
int main(int argc,char** argv){try{
    require(argc==4||argc==5||argc==6,"usage: stream-coverage-fixture VERTEX ORIGINAL COVERAGE [1|3] [CAMERA_ONLY]");
    const unsigned descriptorCount=argc>=5?unsigned(std::stoul(argv[4])):3;
    require(descriptorCount==1||descriptorCount==3,"explicit count1/count3 layout required");
    CoverageGpu gpu;gpu.init();require(gpu.validationEnabled,"real validation enabled");gpu.singleResources(descriptorCount);gpu.prepareMask();gpu.pipelinesSingle(argv[1],argv[2],argv[3],descriptorCount==1);
    std::unique_ptr<CoverageGpu> oracle;
    if(descriptorCount==1){oracle=std::make_unique<CoverageGpu>();oracle->init();oracle->singleResources(3);oracle->prepareMask();oracle->pipelinesSingle(argv[1],argv[2],argv[2]);}
    unsigned count=0,zeroEmission=0;
    for(auto c:cases())for(unsigned band=0;band<4;++band){
        c.u[0]=(band*4+2.5f)/16.f;c.u[1]=.5f;c.r1={0,0,1,1};
        const auto original=oracle?oracle->drawSingle(c,oracle->original):gpu.drawSingle(c,gpu.original),covered=gpu.drawMasked(c);
        require(std::equal(original.begin(),original.begin()+3,covered.begin()),"full native RGB remains bit identical");
        auto sample=[](float u){const int x=std::max(0,std::min(15,int(std::floor(u*16.f))));return std::array<unsigned,4>{{0,64,127,255}}[unsigned(x)/4]/255.f;};
        const float ix=(c.u[0]-c.r0[0])/(c.r0[2]-c.r0[0]),iy=(c.u[1]-c.r0[1])/(c.r0[3]-c.r0[1]);
        const float weight=(c.flags>>16)&2?0.f:1.f-std::max(0.f,std::min(1.f,(std::max(std::abs(ix*2-1),std::abs(iy*2-1))-.800000012f)*5.26315784f));
        const auto expected=sample(c.u[0])*(1-weight)+sample(c.patch[0]+ix*(c.r1[2]-c.r1[0]))*weight;
        near(floating(covered[3]),expected,"independent R8 alpha at exact native coded UV");
        if(expected==0&&(floating(covered[0])!=0||floating(covered[1])!=0||floating(covered[2])!=0))++zeroEmission;
        const auto ordinary=gpu.drawMasked(c,0);
        require(ordinary==original,"invalidated recording falls back to exact original RGBA");
        const auto recovery=gpu.drawMasked(c,0,1);
        require(recovery==std::array<std::uint32_t,4>{},"invalid exact data with active recovery emits zero RGBA: camera only, no game RGB");
        require(gpu.drawMasked(c,1,1)==covered,"active recovery cannot alter one bit of a valid normal game picture");
        ++count;
    }
    if(argc==6){
        CoverageGpu camera;camera.init();camera.singleResources(descriptorCount);camera.prepareMask();camera.pipelinesSingle(argv[1],argv[2],argv[5],true);
        for(auto c:cases()){
            require(camera.drawMasked(c,0,1,false)==std::array<std::uint32_t,4>{},"standalone recovery emits zero RGBA with descriptor set1 unbound");
            const auto original=oracle?oracle->drawSingle(c,oracle->original):gpu.drawSingle(c,gpu.original);
            require(camera.drawMasked(c,0,0,false)==original,"standalone recovery withdrawal restores original RGBA without any mask descriptor");
        }
    }
    require(zeroEmission>0,"actual E>0/A0 is preserved");require(validationErrors.load()==0,"zero Vulkan validation errors");
    std::cout<<"{\"schema\":\"sfnp-native-stream-coverage-gpu-v1\",\"cases\":"<<count<<",\"checks\":"<<checks<<",\"rgb_bit_identical\":true,\"validation_errors\":"<<validationErrors.load()<<",\"native_source_selected_binding_proven\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
