// Executes the locally transformed native fragment with an independent R8 mask.
// Native RGB has an independent unchanged-original GPU oracle in every case.
#define SFNP_R1_FIXTURE_NO_MAIN
#include "../native-fragment-r1-presenter-gpu/fixture.cpp"
namespace {
struct CoverageGpu:OutputGpu {
    VkDescriptorSetLayout maskLayout{};VkDescriptorPool maskPool{};VkDescriptorSet maskSet{};
    std::size_t maskImage=0,maskUpload=0;
    ~CoverageGpu(){if(maskPool)vkDestroyDescriptorPool(device,maskPool,nullptr);if(maskLayout)vkDestroyDescriptorSetLayout(device,maskLayout,nullptr);}
    void prepareMask(){
        maskImage=image(VK_FORMAT_R8_UNORM,16,16,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        maskUpload=buffer(256,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};l.bindingCount=1;l.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&l,nullptr,&maskLayout));
        vkDestroyPipelineLayout(device,pipelineLayout,nullptr);pipelineLayout={};
        VkDescriptorSetLayout layouts[2]{descriptorLayout,maskLayout};
        VkPipelineLayoutCreateInfo p{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};p.setLayoutCount=2;p.pSetLayouts=layouts;
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT,0,16};p.pushConstantRangeCount=1;p.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device,&p,nullptr,&pipelineLayout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&maskPool));
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=maskPool;a.descriptorSetCount=1;a.pSetLayouts=&maskLayout;
        check(vkAllocateDescriptorSets(device,&a,&maskSet));
        VkDescriptorImageInfo info{sampler,images[maskImage].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=maskSet;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info;
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
    std::array<std::uint32_t,4> drawMasked(const Case& c){metadata(c);startPass();vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,&maskSet,0,nullptr);raster(probe,c.u);return readPass();}
};
}
int main(int argc,char** argv){try{
    require(argc==4,"usage: stream-coverage-fixture VERTEX ORIGINAL COVERAGE");
    CoverageGpu gpu;gpu.init();require(gpu.validationEnabled,"real validation enabled");gpu.singleResources(3);gpu.prepareMask();gpu.pipelinesSingle(argv[1],argv[2],argv[3]);
    unsigned count=0,zeroEmission=0;
    for(auto c:cases())for(unsigned band=0;band<4;++band){
        c.u[0]=(band*4+2.5f)/16.f;c.u[1]=.5f;c.r1={0,0,1,1};
        const auto original=gpu.drawSingle(c,gpu.original),covered=gpu.drawMasked(c);
        require(std::equal(original.begin(),original.begin()+3,covered.begin()),"full native RGB remains bit identical");
        const auto expected=std::array<unsigned,4>{{0,64,127,255}}[band]/255.f;
        near(floating(covered[3]),expected,"independent R8 alpha at exact native coded UV");
        if(!band&&(floating(covered[0])!=0||floating(covered[1])!=0||floating(covered[2])!=0))++zeroEmission;
        ++count;
    }
    require(zeroEmission>0,"actual E>0/A0 is preserved");require(validationErrors.load()==0,"zero Vulkan validation errors");
    std::cout<<"{\"schema\":\"sfnp-native-stream-coverage-gpu-v1\",\"cases\":"<<count<<",\"checks\":"<<checks<<",\"rgb_bit_identical\":true,\"validation_errors\":"<<validationErrors.load()<<",\"native_source_selected_binding_proven\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
