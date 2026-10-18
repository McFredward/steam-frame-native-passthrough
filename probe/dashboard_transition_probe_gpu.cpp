// Owned offscreen execution only. This fixture enables fragment stores on its
// own test device; the attachable layer NEVER enables a native device feature.
#include <vulkan/vulkan.h>
#include "dashboard_transition_probe.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
VkResult createDashboardFixtureDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* input,const VkAllocationCallbacks* alloc,VkDevice* out){
    VkPhysicalDeviceFeatures supported{};vkGetPhysicalDeviceFeatures(physical,&supported);
    if(!input->pEnabledFeatures||!supported.fragmentStoresAndAtomics)return VK_ERROR_FEATURE_NOT_PRESENT;
    auto enabled=*input->pEnabledFeatures;enabled.fragmentStoresAndAtomics=VK_TRUE;
    auto info=*input;info.pEnabledFeatures=&enabled;return vkCreateDevice(physical,&info,alloc,out);
}
unsigned dashboardBarrierReceipts=0;
PFN_vkCmdPipelineBarrier actualDashboardBarrier{};
VKAPI_ATTR void VKAPI_CALL checkedDashboardBarrier(VkCommandBuffer command,VkPipelineStageFlags source,VkPipelineStageFlags destination,VkDependencyFlags dependencies,
    std::uint32_t memoryCount,const VkMemoryBarrier* memories,std::uint32_t bufferCount,const VkBufferMemoryBarrier* buffers,std::uint32_t imageCount,const VkImageMemoryBarrier* images){
    if(destination&VK_PIPELINE_STAGE_HOST_BIT){
        if(!(source&VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)||!(source&VK_PIPELINE_STAGE_TRANSFER_BIT)||
           !(destination&VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)||bufferCount!=1||
           !(buffers[0].srcAccessMask&VK_ACCESS_SHADER_WRITE_BIT)||!(buffers[0].srcAccessMask&VK_ACCESS_TRANSFER_WRITE_BIT)||
           !(buffers[0].dstAccessMask&VK_ACCESS_HOST_READ_BIT)||!(buffers[0].dstAccessMask&VK_ACCESS_SHADER_READ_BIT))
            throw std::runtime_error("terminal barrier must expose payload to host AND phase3 to future fragments");
        ++dashboardBarrierReceipts;
    }
    actualDashboardBarrier(command,source,destination,dependencies,memoryCount,memories,bufferCount,buffers,imageCount,images);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL checkedDashboardDispatch(VkDevice device,const char* name){
    const auto next=vkGetDeviceProcAddr(device,name);
    if(!std::strcmp(name,"vkCmdPipelineBarrier")){actualDashboardBarrier=reinterpret_cast<PFN_vkCmdPipelineBarrier>(next);return reinterpret_cast<PFN_vkVoidFunction>(checkedDashboardBarrier);}
    return next;
}
#define vkCreateDevice createDashboardFixtureDevice
#include "borrowed_dashboard_guard_fixture.h"
#undef vkCreateDevice

namespace {
struct DashboardGpu:GuardGpu {
    VkDescriptorSetLayout exportLayout{};VkDescriptorPool exportPool{};VkDescriptorSet exportSet{};
    std::size_t output{};
    VkBuffer actualRecordBuffer{};
    ~DashboardGpu(){if(device){vkDeviceWaitIdle(device);if(exportPool)vkDestroyDescriptorPool(device,exportPool,nullptr);if(exportLayout)vkDestroyDescriptorSetLayout(device,exportLayout,nullptr);}}
    void setup(){
        guardResources();output=buffer(1664,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(buffers[output].mapped,0,1600);std::memset(static_cast<char*>(buffers[output].mapped)+1600,0xa5,64);
        static_cast<std::uint32_t*>(buffers[output].mapped)[0]=3;flush(output);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo l{};l.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;l.bindingCount=1;l.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&l,nullptr,&exportLayout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&exportPool));
        VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;a.descriptorPool=exportPool;a.descriptorSetCount=1;a.pSetLayouts=&exportLayout;
        check(vkAllocateDescriptorSets(device,&a,&exportSet));
        VkDescriptorBufferInfo b{buffers[output].value,0,1664};VkWriteDescriptorSet w{};w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=exportSet;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&b;
        vkUpdateDescriptorSets(device,1,&w,0,nullptr);
        vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        VkDescriptorSetLayout layouts[2]{descriptorLayout,exportLayout};VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,16};
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=2;pl.pSetLayouts=layouts;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
    }
    std::array<std::uint32_t,4> run(const Case& c,VkPipeline pipeline,unsigned phase){
        metadata(c);auto* words=static_cast<std::uint32_t*>(buffers[output].mapped);words[0]=phase;flush(output);
        set=sourceSets[0];startPass();vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,&exportSet,0,nullptr);
        raster(pipeline,c.u);vkCmdEndRenderPass(command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(command,images[targets[0]].value,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[readback].value,1,&copy);
        VkBufferMemoryBarrier barriers[2]{};for(auto& b:barriers){b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;}
        barriers[0].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barriers[0].buffer=buffers[readback].value;barriers[0].size=16;
        barriers[1].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barriers[1].buffer=actualRecordBuffer?actualRecordBuffer:buffers[output].value;barriers[1].size=actualRecordBuffer?1600:1664;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,2,barriers,0,nullptr);
        finish();
        for(auto index:{readback,output})if(!buffers[index].coherent){VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=buffers[index].memory;r.size=VK_WHOLE_SIZE;check(vkInvalidateMappedMemoryRanges(device,1,&r));}
        std::array<std::uint32_t,4> result{};std::memcpy(result.data(),buffers[readback].mapped,16);
        const auto* canary=static_cast<const unsigned char*>(buffers[output].mapped)+1600;
        require(std::all_of(canary,canary+64,[](unsigned char v){return v==0xa5;}),"no sparse-buffer overflow");
        return result;
    }
};
std::vector<float> ownedTexture(bool valid){
    const unsigned origins[12][2]{{48,64},{208,64},{368,64},{48,224},{208,224},{368,224},{2672,2704},{2832,2704},{2992,2704},{2672,2864},{2832,2864},{2992,2864}};
    const unsigned codes[12]{32,224,224,96,96,160,96,224,160,224,96,96};
    std::vector<float> out(SourceWidth*SourceHeight*4);
    const auto color=encodeColor({.25f,.375f,.125f,1},0x02038018);
    for(unsigned y=0;y<SourceHeight;++y)for(unsigned x=0;x<SourceWidth;++x){
        const float px=(x+.5f)*3168/SourceWidth,py=(y+.5f)*3168/SourceHeight;
        Vec value{.3f,.5f,.7f,1};
        if(px>=560&&px<2608&&py>=560&&py<2608)value=color;
        for(unsigned i=0;i<12;++i)if(px>=origins[i][0]&&px<origins[i][0]+128&&py>=origins[i][1]&&py<origins[i][1]+128)value={.3f,valid?codes[i]/255.f:0.f,.7f,1};
        std::copy(value.begin(),value.end(),out.begin()+4*(y*SourceWidth+x));
    }
    return out;
}
}
namespace dashboard_transition_probe {
struct TestAccess {
    static bool request(Probe& p,unsigned value){return p.sequence_.request(value);}
    static VkDescriptorSet descriptor(Probe& p){return p.descriptor_;}
    static VkBuffer buffer(Probe& p){return p.buffer_;}
    static std::array<std::uint32_t,400> copy(Probe& p){std::array<std::uint32_t,400> out;std::memcpy(out.data(),p.mapped_,1600);return out;}
};
}
int main(int argc,char** argv){try{
    require(argc==5||argc==6,"vertex/shipping/diagnostic/prepared-run explicit private inputs");const bool lifecycleOnly=argc==6&&std::strcmp(argv[5],"lifecycle")==0;
    DashboardGpu gpu;gpu.init();gpu.setup();gpu.guardPipelines(argv[1],argv[2],argv[3],false);
    const Vec chart{0,0,1,1};Case c{0,0x02038018,{.5f,.5f,0,0},chart,chart,{1,1,0,0},{1,1,1,1}};
    gpu.update(0,ownedTexture(true));
    if(!lifecycleOnly){
    const auto initial=gpu.run(c,gpu.probe,3);const auto baseline=gpu.run(c,gpu.original,3);
    require(initial==baseline,"initial disabled diagnostic output is bit-identical raster");
    const auto* words=static_cast<const std::uint32_t*>(gpu.buffers[gpu.output].mapped);
    for(unsigned i=1;i<400;++i)require(words[i]==0,"initial disabled never claims or writes slots");
    for(unsigned phase=0;phase<3;++phase)for(unsigned eye=0;eye<2;++eye){
        c.eye=eye;gpu.update(0,ownedTexture(phase!=1));
        const auto normal=gpu.run(c,gpu.original,3),observed=gpu.run(c,gpu.probe,phase);
        require(normal==observed,"actual diagnostic and shipping color raster bit-identical");
        const auto* record=words+16+64*(phase*2+eye);
        require(record[0]==0x44545031&&record[2]==phase&&record[3]==eye&&record[63]==1,"central receipt exact phase/consumed-selector/R1");
        require(record[4]==(phase==1?0u:3u),"guard reject versus decoded v3 path distinguished");
        const auto before=std::vector<std::uint32_t>(record,record+64);
        c.tint[0]=.75f;gpu.run(c,gpu.probe,phase);c.tint[0]=1;
        require(std::equal(before.begin(),before.end(),record),"spent slot never overwritten by later invocation");
    }
    }
    // Exercise the ACTUAL queue-ordered control commands and owned resources.
    // The render command is recorded once, then reused for all six executions.
    dashboard_transition_probe::Probe live;
    const int directory=open(argv[4],O_RDONLY|O_DIRECTORY|O_CLOEXEC);require(directory>=0,"private prepared GPU run");
    float priority=1;VkDeviceQueueCreateInfo queueInfo{};queueInfo.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;queueInfo.queueFamilyIndex=gpu.family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures enabled{};enabled.geometryShader=enabled.fragmentStoresAndAtomics=VK_TRUE;
    VkDeviceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;info.queueCreateInfoCount=1;info.pQueueCreateInfos=&queueInfo;info.pEnabledFeatures=&enabled;
    live.prepare(gpu.device,info,checkedDashboardDispatch,gpu.memory,directory,VK_QUEUE_GRAPHICS_BIT);close(directory);
    require(live.prepared()&&live.refusal()==dashboard_transition_probe::Refusal::None,"actual feature-enabled owned resource preparation");
    live.queueRetrieved(gpu.family,0,gpu.queue);
    VkGraphicsPipelineCreateInfo observed{};observed.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    for(unsigned i=0;i<32;++i){VkPipeline handle{};const std::uint64_t raw=1000+i;std::memcpy(&handle,&raw,sizeof(handle));live.pipelineObserved(handle,observed,99);}
    gpu.exportSet=dashboard_transition_probe::TestAccess::descriptor(live);gpu.actualRecordBuffer=dashboard_transition_probe::TestAccess::buffer(live);
    gpu.set=gpu.sourceSets[0];
    gpu.update(0,ownedTexture(true));c.eye=0;c.tint[0]=1;gpu.metadata(c);
    check(vkResetCommandBuffer(gpu.command,0));VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    check(vkBeginCommandBuffer(gpu.command,&begin));VkClearValue clear{};
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;pass.renderPass=gpu.renderPass;pass.framebuffer=gpu.framebuffer;pass.renderArea={{0,0},{1,1}};pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(gpu.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(gpu.command,VK_PIPELINE_BIND_POINT_GRAPHICS,gpu.pipelineLayout,1,1,&gpu.exportSet,0,nullptr);gpu.raster(gpu.probe,c.u);vkCmdEndRenderPass(gpu.command);
    VkBufferMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=gpu.actualRecordBuffer;host.size=1600;
    vkCmdPipelineBarrier(gpu.command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);check(vkEndCommandBuffer(gpu.command));
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&gpu.command;
    live.beforeSubmit(gpu.queue,true,1,&submit);
    check(vkResetFences(gpu.device,1,&gpu.fence));check(vkQueueSubmit(gpu.queue,1,&submit,gpu.fence));check(vkWaitForFences(gpu.device,1,&gpu.fence,VK_TRUE,10000000000ull));
    auto records=dashboard_transition_probe::TestAccess::copy(live);
    require(records[1]==0&&records[16]==0,"initial disabled resource executes reused draw without collection");
    for(unsigned phase=0;phase<3;++phase){
        require(dashboard_transition_probe::TestAccess::request(live,phase),"actual monotonic GPU phase requested");
        for(unsigned eye=0;eye<2;++eye){
            c.eye=eye;gpu.metadata(c);live.beforeSubmit(gpu.queue,true,1,&submit);
            check(vkResetFences(gpu.device,1,&gpu.fence));check(vkQueueSubmit(gpu.queue,1,&submit,gpu.fence));check(vkWaitForFences(gpu.device,1,&gpu.fence,VK_TRUE,10000000000ull));
            records=dashboard_transition_probe::TestAccess::copy(live);const auto offset=16+64*(phase*2+eye);
            require(records[offset]==0x44545031&&records[offset+2]==phase&&records[offset+3]==eye,"same recorded draw consumes current GPU phase and selector");
        }
    }
    require(dashboard_transition_probe::TestAccess::request(live,3),"explicit terminal GPU phase");live.beforeSubmit(gpu.queue,true,1,&submit);
    check(vkQueueWaitIdle(gpu.queue));const auto stopped=dashboard_transition_probe::TestAccess::copy(live);
    require(dashboardBarrierReceipts==1,"actual final barrier exact fragment/host visibility captured");
    for(unsigned i=0;i<4;++i){
        live.beforeSubmit(gpu.queue,true,1,&submit);check(vkResetFences(gpu.device,1,&gpu.fence));
        check(vkQueueSubmit(gpu.queue,1,&submit,gpu.fence));check(vkWaitForFences(gpu.device,1,&gpu.fence,VK_TRUE,10000000000ull));
        require(dashboard_transition_probe::TestAccess::copy(live)==stopped,"no slot writes after explicit disable despite reused draw execution");
    }
    // Bounded cold polling verifies the terminal receipt survives more than24
    // unrelated pipeline revisions. This waits only in an owned host fixture.
    bool terminal=false;const int terminalDirectory=open(argv[4],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    for(unsigned i=0;i<40&&!terminal;++i){
        const int file=openat(terminalDirectory,"dashboard-transition-terminal.json",O_RDONLY|O_CLOEXEC);
        if(file>=0){close(file);terminal=true;}else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close(terminalDirectory);
    require(terminal,"completed terminal receipt remains available after32 cold route revisions");
    live.release(VK_SUCCESS);
    require(validationErrors.load()==0,"actual GPU validation stays clean");
    std::cout<<"{\"checks\":"<<checks<<",\"raster_bit_identical\":true,\"actual_reused_cb_phase_updates\":true,\"slots\":6,\"validation_errors\":"<<validationErrors.load()<<",\"native_frame_or_eye_proven\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
