#include "dashboard_transition_probe.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <limits>
#include <map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace dashboard_transition_probe {
namespace {
unsigned checks=0,submits=0;
unsigned queryMode=0,createBufferCalls=0;
std::vector<unsigned> operations;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class T>T h(std::uint64_t value){T out{};std::memcpy(&out,&value,sizeof(out));return out;}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,std::uint32_t count,const VkSubmitInfo* input,VkFence){
    check(count==1&&input&&input->commandBufferCount==1,"own bounded phase-only submit");++submits;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline value){operations.push_back(unsigned(frame_pixel_ssbo_clone::handle(value)));}
VKAPI_ATTR void VKAPI_CALL bindSet(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet*,std::uint32_t dynamic,const std::uint32_t*){
    check(first==1&&count==1&&!dynamic,"Set0 is never rebound or disturbed");operations.push_back(77);
}
VKAPI_ATTR void VKAPI_CALL fakeQueueQuery(VkPhysicalDevice,std::uint32_t* count,VkQueueFamilyProperties* values){
    if(!values){*count=queryMode==1?0:queryMode==2?65:9;return;}
    if(queryMode==3){*count=8;return;}
    for(unsigned i=0;i<*count;++i)values[i].queueFlags=queryMode==4?VK_QUEUE_COMPUTE_BIT:VK_QUEUE_GRAPHICS_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL failingCreateBuffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*){
    ++createBufferCalls;return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
VKAPI_ATTR void VKAPI_CALL unusedDriverFunction(){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeDispatch(VkDevice,const char* name){
    return !std::strcmp(name,"vkCreateBuffer")?reinterpret_cast<PFN_vkVoidFunction>(failingCreateBuffer):unusedDriverFunction;
}
std::string readFile(const std::string& path){std::ifstream input(path);return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
struct PrivateRun {
    std::string directory;int fd=-1;
    explicit PrivateRun(const std::vector<char>& artifact){
        char pattern[]="/tmp/dashboard-queue-vector-XXXXXX";const auto* made=mkdtemp(pattern);check(made,"private queue-vector directory");directory=made;
        fd=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);check(fd>=0,"private directory descriptor");
        const auto put=[&](const char* name,const void* data,std::size_t size){const int file=openat(fd,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);check(file>=0,"owned fixture input");const auto written=write(file,data,size);close(file);check(written==static_cast<ssize_t>(size),"whole bounded fixture input");};
        const char magic[]="SFNP_DASHBOARD_TRANSITION/1\n";put("dashboard-transition.enable",magic,sizeof(magic)-1);put("dashboard-transition.spv",artifact.data(),artifact.size());
    }
    ~PrivateRun(){if(fd>=0){
        for(const char* name:{"dashboard-transition.enable","dashboard-transition.spv","dashboard-transition-queue-profile.json",
            "dashboard-transition.phase","dashboard-transition.phase.tmp","dashboard-transition-terminal.json",
            "dashboard-transition-records.bin","dashboard-transition-routes.bin","dashboard-transition-unknown-routes.bin"})unlinkat(fd,name,0);
        for(unsigned n=0;n<24;++n){char name[64];std::snprintf(name,sizeof(name),"dashboard-transition-status-%02u.json",n);unlinkat(fd,name,0);}
        close(fd);}if(!directory.empty())rmdir(directory.c_str());}
};
// Typed successful driver model. It exercises the real owned-resource setup,
// companion creation and fenced collection; it is not a GPU shader execution.
struct OwnedDriver {
    std::uint64_t next=1000;
    alignas(16) std::array<std::uint32_t,BufferBytes/4> memory{};
    std::map<std::uint64_t,unsigned> updates;
    std::array<std::atomic<bool>,4> fences{};
    std::array<VkFence,4> fenceHandles{};
    unsigned fenceCount=0,createdPipelines=0,destroyedPipelines=0,phaseSubmits=0;
};
OwnedDriver* driver=nullptr;
#define OWN_CREATE(Name,Info,Handle) \
VKAPI_ATTR VkResult VKAPI_CALL own##Name(VkDevice,const Info*,const VkAllocationCallbacks*,Handle* out){*out=h<Handle>(driver->next++);return VK_SUCCESS;}
#define OWN_DESTROY(Name,Handle) \
VKAPI_ATTR void VKAPI_CALL own##Name(VkDevice,Handle,const VkAllocationCallbacks*){}
OWN_CREATE(CreateBuffer,VkBufferCreateInfo,VkBuffer) OWN_DESTROY(DestroyBuffer,VkBuffer)
OWN_CREATE(AllocateMemory,VkMemoryAllocateInfo,VkDeviceMemory) OWN_DESTROY(FreeMemory,VkDeviceMemory)
OWN_CREATE(CreateDescriptorSetLayout,VkDescriptorSetLayoutCreateInfo,VkDescriptorSetLayout) OWN_DESTROY(DestroyDescriptorSetLayout,VkDescriptorSetLayout)
OWN_CREATE(CreateDescriptorPool,VkDescriptorPoolCreateInfo,VkDescriptorPool) OWN_DESTROY(DestroyDescriptorPool,VkDescriptorPool)
OWN_CREATE(CreatePipelineLayout,VkPipelineLayoutCreateInfo,VkPipelineLayout) OWN_DESTROY(DestroyPipelineLayout,VkPipelineLayout)
OWN_CREATE(CreateShaderModule,VkShaderModuleCreateInfo,VkShaderModule) OWN_DESTROY(DestroyShaderModule,VkShaderModule)
OWN_CREATE(CreateCommandPool,VkCommandPoolCreateInfo,VkCommandPool) OWN_DESTROY(DestroyCommandPool,VkCommandPool)
OWN_DESTROY(DestroyFence,VkFence)
#undef OWN_CREATE
#undef OWN_DESTROY
VKAPI_ATTR void VKAPI_CALL ownGetBufferMemoryRequirements(VkDevice,VkBuffer,VkMemoryRequirements* out){out->size=BufferBytes;out->alignment=16;out->memoryTypeBits=1;}
VKAPI_ATTR VkResult VKAPI_CALL ownBindBufferMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ownMapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void** out){*out=driver->memory.data();return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL ownUnmapMemory(VkDevice,VkDeviceMemory){}
VKAPI_ATTR VkResult VKAPI_CALL ownAllocateDescriptorSets(VkDevice,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out){for(unsigned i=0;i<info->descriptorSetCount;++i)out[i]=h<VkDescriptorSet>(driver->next++);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL ownUpdateDescriptorSets(VkDevice,std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*){}
VKAPI_ATTR VkResult VKAPI_CALL ownCreateGraphicsPipelines(VkDevice,VkPipelineCache,std::uint32_t count,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline* out){for(unsigned i=0;i<count;++i){out[i]=h<VkPipeline>(driver->next++);++driver->createdPipelines;}return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL ownDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*){++driver->destroyedPipelines;}
VKAPI_ATTR VkResult VKAPI_CALL ownAllocateCommandBuffers(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){for(unsigned i=0;i<info->commandBufferCount;++i)out[i]=h<VkCommandBuffer>(driver->next++);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ownBeginCommandBuffer(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ownEndCommandBuffer(VkCommandBuffer){return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL ownCmdUpdateBuffer(VkCommandBuffer command,VkBuffer,VkDeviceSize offset,VkDeviceSize size,const void* input){check(!offset&&size==4,"phase update has bounded exact control word");driver->updates[frame_pixel_ssbo_clone::handle(command)]=*static_cast<const unsigned*>(input);}
VKAPI_ATTR void VKAPI_CALL ownCmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t,const VkImageMemoryBarrier*){}
VKAPI_ATTR VkResult VKAPI_CALL ownCreateFence(VkDevice,const VkFenceCreateInfo*,const VkAllocationCallbacks*,VkFence* out){*out=h<VkFence>(driver->next++);driver->fenceHandles[driver->fenceCount++]=*out;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL ownGetFenceStatus(VkDevice,VkFence value){for(unsigned i=0;i<4;++i)if(driver->fenceHandles[i]==value)return driver->fences[i].load(std::memory_order_acquire)?VK_SUCCESS:VK_NOT_READY;return VK_ERROR_UNKNOWN;}
VKAPI_ATTR VkResult VKAPI_CALL ownQueueSubmit(VkQueue,std::uint32_t count,const VkSubmitInfo* info,VkFence fence){
    check(count==1&&info&&info->commandBufferCount==1,"real probe submits its own one bounded phase command");
    const auto phase=driver->updates.at(frame_pixel_ssbo_clone::handle(info->pCommandBuffers[0]));
    driver->memory[0]=phase;++driver->phaseSubmits;
    for(unsigned i=0;i<4;++i)if(driver->fenceHandles[i]==fence)driver->fences[i].store(true,std::memory_order_release);
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ownedDispatch(VkDevice,const char* name){
#define OWN_LOAD(Name) if(!std::strcmp(name,"vk" #Name))return reinterpret_cast<PFN_vkVoidFunction>(own##Name);
    OWN_LOAD(CreateBuffer) OWN_LOAD(DestroyBuffer) OWN_LOAD(GetBufferMemoryRequirements)
    OWN_LOAD(AllocateMemory) OWN_LOAD(FreeMemory) OWN_LOAD(BindBufferMemory) OWN_LOAD(MapMemory) OWN_LOAD(UnmapMemory)
    OWN_LOAD(CreateDescriptorSetLayout) OWN_LOAD(DestroyDescriptorSetLayout) OWN_LOAD(CreateDescriptorPool) OWN_LOAD(DestroyDescriptorPool)
    OWN_LOAD(AllocateDescriptorSets) OWN_LOAD(UpdateDescriptorSets) OWN_LOAD(CreatePipelineLayout) OWN_LOAD(DestroyPipelineLayout)
    OWN_LOAD(CreateShaderModule) OWN_LOAD(DestroyShaderModule) OWN_LOAD(CreateGraphicsPipelines) OWN_LOAD(DestroyPipeline)
    OWN_LOAD(CreateCommandPool) OWN_LOAD(DestroyCommandPool) OWN_LOAD(AllocateCommandBuffers) OWN_LOAD(BeginCommandBuffer) OWN_LOAD(EndCommandBuffer)
    OWN_LOAD(CmdUpdateBuffer) OWN_LOAD(CmdPipelineBarrier) OWN_LOAD(CreateFence) OWN_LOAD(DestroyFence) OWN_LOAD(GetFenceStatus) OWN_LOAD(QueueSubmit)
#undef OWN_LOAD
    if(!std::strcmp(name,"vkCmdBindPipeline"))return reinterpret_cast<PFN_vkVoidFunction>(bindPipeline);
    if(!std::strcmp(name,"vkCmdBindDescriptorSets"))return reinterpret_cast<PFN_vkVoidFunction>(bindSet);
    return nullptr;
}
void prepareOwned(Probe& probe,PrivateRun& run){
    float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=7;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures features{};features.fragmentStoresAndAtomics=VK_TRUE;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};info.queueCreateInfoCount=1;info.pQueueCreateInfos=&queue;info.pEnabledFeatures=&features;
    VkPhysicalDeviceMemoryProperties memory{};memory.memoryTypeCount=1;memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    queryMode=0;const auto observation=observeQueueFamily(fakeQueueQuery,h<VkPhysicalDevice>(3),7);
    probe.prepare(h<VkDevice>(4),info,ownedDispatch,memory,run.fd,observation.flags,&observation);
    check(probe.prepared()&&probe.refusal()==Refusal::None,"actual prepare creates owned resources and starts the real writer");
    probe.queueRetrieved(7,0,h<VkQueue>(5));
}
}
struct TestAccess {
    static void setup(Probe& p){
        p.ready_=true;p.refusal_=Refusal::None;p.queue_=h<VkQueue>(5);
        p.QueueSubmit_=submit;p.CmdBindPipeline_=bindPipeline;p.CmdBindDescriptorSets_=bindSet;
        for(unsigned i=0;i<4;++i){p.phaseCommands_[i]=h<VkCommandBuffer>(100+i);p.phaseFences_[i]=h<VkFence>(200+i);}
        p.pipelines_[0]={h<VkPipeline>(11),h<VkPipeline>(22),h<VkPipelineLayout>(33),44,false};
    }
    static bool request(Probe& p,unsigned phase){return p.sequence_.request(phase);}
    static bool disabledPublished(Probe& p){return p.disabledFenceSubmitted_.load();}
    static unsigned queued(Probe& p){return p.queuedPhase_.load();}
    static void seedOldRoute(Probe& p,VkCommandBuffer value){auto* c=p.lookup(value);c->usedRoutes=0x80;c->unknownRoute=true;}
    static bool cleanRoutes(Probe& p,VkCommandBuffer value){const auto* c=p.lookup(value);return c&&!c->usedRoutes&&!c->unknownRoute;}
    static bool absentCommand(Probe& p,VkCommandBuffer value){return !p.lookup(value);}
    static std::uint64_t droppedShaders(const Probe& p){return p.shaderDropped_.load();}
    static std::uint64_t droppedRoutes(const Probe& p){return p.routesDropped_.load();}
    static unsigned cachedShaders(const Probe& p){return p.shaderCached_.load();}
    static unsigned genericRoutes(const Probe& p){return p.genericRoutes_.load();}
    static unsigned companionUsed(const Probe& p){return p.companionUsed_.load();}
    static unsigned companionCreated(const Probe& p){return p.companionCreated_.load();}
    static auto routes(const Probe& p){return p.routes_;}
    static unsigned requested(const Probe& p){return p.sequence_.requested.load();}
    static bool collected(const Probe& p){return p.collected_.load();}
    static QueueProfile queueProfile(const Probe& p){return p.queueProfile_;}
    static std::uint32_t family(const Probe& p){return p.family_;}
};
int queueVectors(const char* artifactPath){
    std::ifstream input(artifactPath,std::ios::binary);std::vector<char> artifact{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    check(artifact.size()==ShaderBytes,"exact existing pinned private diagnostic artifact");
    for(unsigned mode=0;mode<22;++mode){
        queryMode=0;createBufferCalls=0;
        float priority=1;
        VkDeviceQueueGlobalPriorityCreateInfoKHR global{};global.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR;
        const std::array<VkQueueGlobalPriorityKHR,4> known{{VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR,VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR}};
        VkDeviceQueueCreateInfo queue{};queue.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;queue.queueFamilyIndex=7;queue.queueCount=1;queue.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures enabled{};enabled.fragmentStoresAndAtomics=VK_TRUE;
        VkDeviceCreateInfo device{};device.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;device.pEnabledFeatures=&enabled;
        bool expectedAdmission=mode<5;std::uint32_t expected=0;
        if(mode>=1&&mode<=4){global.globalPriority=known[mode-1];queue.pNext=&global;}
        switch(mode){
        case 5:global.globalPriority=static_cast<VkQueueGlobalPriorityKHR>(0);queue.pNext=&global;expected=QueueGlobalPriority;break;
        case 6:global.sType=VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO;global.pNext=reinterpret_cast<const void*>(1);queue.pNext=&global;expected=QueueNextType|QueueNextChain;break;
        case 7:global.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;global.pNext=reinterpret_cast<const void*>(1);queue.pNext=&global;expected=QueueNextChain;break;
        case 8:queue.flags=VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;expected=QueueFlags;break;
        case 9:queryMode=4;expected=QueueNotGraphics;break;
        case 10:expected=QueueQueryMissing;break;
        case 11:queryMode=1;expected=QueueQueryCount;break;
        case 12:queryMode=2;expected=QueueQueryCount;break;
        case 13:queryMode=3;expected=QueueQueryChanged;break;
        case 14:queue.queueFamilyIndex=9;expected=QueueQueryRange;break;
        case 15:device.pQueueCreateInfos=nullptr;expected=QueueMissing;break;
        case 16:queue.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;expected=QueueType;break;
        case 17:queue.pQueuePriorities=nullptr;expected=QueuePriorityMissing;break;
        case 18:priority=std::numeric_limits<float>::quiet_NaN();expected=QueuePriorityValue;break;
        case 19:priority=1.1f;expected=QueuePriorityValue;break;
        case 20:queue.queueCount=2;expected=QueueCount;break;
        case 21:queue.queueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;expected=QueueFamily|QueueQueryRange;break;
        }
        const auto deviceBefore=device;const auto queueBefore=queue;const auto globalBefore=global;std::uint32_t priorityBefore{};std::memcpy(&priorityBefore,&priority,4);
        const auto rawFamily=device.pQueueCreateInfos?queue.queueFamilyIndex:UINT32_MAX;
        const auto observation=observeQueueFamily(mode==10?nullptr:fakeQueueQuery,h<VkPhysicalDevice>(3),rawFamily);
        PrivateRun run(artifact);
        {
            Probe probe;VkPhysicalDeviceMemoryProperties memory{};
            probe.prepare(h<VkDevice>(4),device,fakeDispatch,memory,run.fd,observation.flags,&observation);
            const auto facts=TestAccess::queueProfile(probe);
            check(expectedAdmission?(probe.refusal()==Refusal::Resource&&createBufferCalls==1):
                  (probe.refusal()==Refusal::QueueProfile&&createBufferCalls==0),"actual prepare admits only safe queue profiles before fake native resource failure");
            check(expectedAdmission?facts.failures==0:(facts.failures&expected)==expected,"actual prepare exposes exact failing predicate bits");
            check(TestAccess::family(probe)==rawFamily,"raw family retained even on refusal; missing family is UINT32_MAX");
            check(facts.query.pfnPresent==(mode!=10)&&facts.query.rawFamily==rawFamily,"query provenance retained separately from queue creation");
            if(mode>=1&&mode<=4)check(facts.globalPriorityObserved&&facts.globalPriority==std::int32_t(known[mode-1]),"each known original scheduling priority retained unchanged");
            for(unsigned retry=0;retry<40&&readFile(run.directory+"/dashboard-transition-status-00.json").empty();++retry)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto status=readFile(run.directory+"/dashboard-transition-status-00.json");
            const auto report=readFile(run.directory+"/dashboard-transition-queue-profile.json");
            check(!status.empty()&&status.size()<1024&&!report.empty()&&report.size()<1024,"actual background status and one-time cold observation each fit existing helper bound");
            check(status.find("\"observed_device\":4")!=std::string::npos&&report.find("\"observed_device\":4")!=std::string::npos,"queue receipt and status belong to the same original successful device");
            if(mode==15)check(report.find("\"raw_family\":4294967295")!=std::string::npos&&status.find("\"native_queue_family\":4294967295")!=std::string::npos,"unavailable family telemetry never becomes zero");
            if(mode==10)check(report.find("\"query_pfn\":false")!=std::string::npos,"missing PFN is visible, not an ambiguous absent graphics bit");
            probe.release(VK_SUCCESS);
        }
        std::uint32_t priorityAfter{};std::memcpy(&priorityAfter,&priority,4);
        check(!std::memcmp(&deviceBefore,&device,sizeof(device))&&!std::memcmp(&queueBefore,&queue,sizeof(queue))&&
              !std::memcmp(&globalBefore,&global,sizeof(global))&&priorityBefore==priorityAfter,"queue observation never mutates original device/queue/priority request");
    }
    std::cout<<"{\"queue_vectors\":22,\"checks\":"<<checks<<",\"actual_prepare_fake_native_resource_failure\":true,\"native_hardware_proven\":false}\n";return 0;
}
namespace {
struct ActualProbe {Probe value;~ActualProbe(){value.release(VK_SUCCESS);}};
std::array<std::uint8_t,32> pin(const char* hex){
    std::array<std::uint8_t,32> result{};
    for(unsigned i=0;i<32;++i){unsigned byte=0;check(std::sscanf(hex+2*i,"%2x",&byte)==1,"exact validated recipe digest");result[i]=static_cast<std::uint8_t>(byte);}
    return result;
}
struct NativeRecipe {
    frame_pixel_ssbo_clone::Recipe recipe{};
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    NativeRecipe(){
        recipe.feature.complete=recipe.feature.enabled=recipe.definitionComplete=true;
        recipe.vertex.complete=recipe.fragment.complete=true;recipe.provenance[0].value=400;
        recipe.vertex.sha256=pin("94abc0c24124e8b4a301e7f1df5a4c8c0e8b0eb50e4c6ea78f3521370a6897c5");
        recipe.fragment.sha256=pin("f28dd5d22a570cbee64057da03d0418b80aee88a8eeed988b2f776e8d9a513f2");
        for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=h<VkShaderModule>(90000+i);stages[i].pName="main";}
        info.stageCount=2;info.pStages=stages.data();info.layout=h<VkPipelineLayout>(401);info.renderPass=h<VkRenderPass>(402);
    }
};
template<class Predicate> bool await(Predicate predicate){
    for(unsigned retry=0;retry<100;++retry){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    return predicate();
}
void phaseFile(PrivateRun& run,unsigned phase){
    const int fd=openat(run.fd,"dashboard-transition.phase.tmp",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    check(fd>=0,"owned atomic manual phase file");const char command[]{char('0'+phase),'\n'};
    check(write(fd,command,2)==2,"whole explicit phase command");close(fd);
    check(!renameat(run.fd,"dashboard-transition.phase.tmp",run.fd,"dashboard-transition.phase"),"atomic deliberate phase publication");
}
}
int censusVectors(const char* artifactPath){
    std::ifstream input(artifactPath,std::ios::binary);std::vector<char> artifact{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    check(artifact.size()==ShaderBytes,"exact pinned diagnostic shader used by actual prepare");
    {
        OwnedDriver native;driver=&native;PrivateRun run(artifact);ActualProbe actual;auto& probe=actual.value;prepareOwned(probe,run);
        const std::array<std::uint32_t,5> words{{0x07230203,0x00010000,0,1,0}};
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=sizeof(words);shader.pCode=words.data();
        for(unsigned i=0;i<512;++i)probe.shaderObserved(h<VkShaderModule>(10000+i),shader);
        NativeRecipe accepted;
        for(unsigned i=0;i<240;++i)probe.pipelineObserved(h<VkPipeline>(20000+i),accepted.info,17);
        check(probe.refusal()==Refusal::None,"unrelated startup metadata exhaustion never disables the prepared diagnostic");
        check(TestAccess::cachedShaders(probe)==128&&TestAccess::droppedShaders(probe)==384,"shader census remains bounded with explicit exact drops");
        check(TestAccess::genericRoutes(probe)==56&&TestAccess::droppedRoutes(probe)==184,"only 56 generic route indices are available with explicit exact drops");
        const auto genericBefore=TestAccess::routes(probe);
        for(unsigned i=0;i<MaximumPipelines;++i)check(!genericBefore[i].handle,"startup cannot occupy reserved companion prefix");
        const auto normal=h<VkPipeline>(30000);
        probe.pipeline(normal,VK_NULL_HANDLE,accepted.info,accepted.recipe);
        probe.pipelineObserved(normal,accepted.info,5);
        const auto routes=TestAccess::routes(probe);
        check(probe.refusal()==Refusal::None&&native.createdPipelines==1&&TestAccess::companionUsed(probe)==1&&TestAccess::companionCreated(probe)==1,"late actual companion creation remains independent of full generic caches");
        check(routes[0].handle==30000&&routes[0].tag==TagTemplate+1&&routes[0].nativeReason==5&&routes[0].reserved==1,"first actual companion owns its stable reserved route");
        check(routes[0].vs==accepted.recipe.vertex.sha256&&routes[0].ps==accepted.recipe.fragment.sha256,"accepted source pins come directly from validated recipe even when native shader handles were never cached");
        for(unsigned i=GenericRouteBegin;i<64;++i)check(!std::memcmp(&routes[i],&genericBefore[i],sizeof(routes[i])),"late companion cannot evict or relabel any used generic route index");
        VkCommandBuffer command=h<VkCommandBuffer>(60000);VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=h<VkCommandPool>(60001);allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        probe.commandsAllocated(allocate,&command,VK_SUCCESS);VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        probe.begin(command,begin,VK_SUCCESS);probe.bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,normal);
        operations.clear();check(probe.beforeDraw(command),"late accepted companion instruments through actual recorded-command path");
        operations.push_back(99);probe.afterDraw(command);probe.end(command,VK_SUCCESS);
        check(operations.size()==4&&operations[1]==77&&operations[2]==99&&operations[3]==30000,"same draw and native pipeline restoration survive startup census truncation");
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};submitInfo.commandBufferCount=1;submitInfo.pCommandBuffers=&command;
        probe.beforeSubmit(h<VkQueue>(5),true,1,&submitInfo);check(native.phaseSubmits==0,"disabled startup never spends final collection fence");
        for(unsigned phase=0;phase<4;++phase){
            phaseFile(run,phase);check(await([&]{return TestAccess::requested(probe)==phase;}),"real writer accepts every explicit ordered phase after census overflow");
            probe.beforeSubmit(h<VkQueue>(5),true,1,&submitInfo);
            check(TestAccess::queued(probe)==phase&&native.phaseSubmits==phase+1&&native.memory[0]==phase,"actual phase resources submit once in sequence");
            probe.beforeSubmit(h<VkQueue>(5),true,1,&submitInfo);check(native.phaseSubmits==phase+1,"reused native submit never rearms a spent phase");
        }
        check(await([&]{return !readFile(run.directory+"/dashboard-transition-terminal.json").empty();}),"real completed final fence yields guaranteed terminal receipt");
        const auto terminal=readFile(run.directory+"/dashboard-transition-terminal.json");
        check(terminal.size()<1024&&terminal.find("\"collected\":true")!=std::string::npos&&terminal.find("\"refusal\":0")!=std::string::npos,"terminal collection remains usable by existing bounded helper");
        for(const char* expected:{"\"companion_used\":1","\"companion_created\":1","\"shader_cached\":128","\"generic_routes\":56","\"shader_dropped\":384","\"routes_dropped\":184","\"shader_truncated\":true","\"routes_truncated\":true"})check(terminal.find(expected)!=std::string::npos,"bounded terminal status distinguishes metadata truncation from actual companion occupancy");
        check(readFile(run.directory+"/dashboard-transition-records.bin").size()==BufferBytes&&readFile(run.directory+"/dashboard-transition-routes.bin").size()==64*sizeof(routes[0]),"actual resource collection retains unchanged bounded binary schemas");
        const auto finalRoutes=TestAccess::routes(probe);check(finalRoutes[0].draws==std::array<std::uint64_t,3>{{2,2,2}},"all three manual phases preserve accepted route usage despite overflow");
        unsigned statusCount=0;for(unsigned i=0;i<24;++i){char name[64];std::snprintf(name,sizeof(name),"/dashboard-transition-status-%02u.json",i);const auto status=readFile(run.directory+name);if(!status.empty()){++statusCount;check(status.size()<1024,"every status fits unchanged parser limit");}}
        check(statusCount<24,"hundreds of dropped census events never spend all phase-control statuses");
        actual.value.release(VK_SUCCESS);check(native.destroyedPipelines==native.createdPipelines,"actual companion owned resources are released quiescently");
    }
    {
        OwnedDriver native;driver=&native;PrivateRun run(artifact);ActualProbe actual;auto& probe=actual.value;prepareOwned(probe,run);NativeRecipe accepted;
        for(unsigned i=0;i<MaximumPipelines;++i){
            probe.pipeline(h<VkPipeline>(70000+i),VK_NULL_HANDLE,accepted.info,accepted.recipe);
            check(probe.refusal()==Refusal::None&&native.createdPipelines==i+1,"each of exactly eight actual companions remains admitted");
        }
        const auto before=TestAccess::routes(probe);probe.pipelineRetiring(h<VkPipeline>(70000));
        probe.pipeline(h<VkPipeline>(80000),VK_NULL_HANDLE,accepted.info,accepted.recipe);
        check(probe.refusal()==Refusal::PipelineCapacity&&native.createdPipelines==8&&TestAccess::companionUsed(probe)==8&&TestAccess::companionCreated(probe)==8,"ninth actual companion remains fatal even after an old native pipeline retires");
        const auto after=TestAccess::routes(probe);for(unsigned i=0;i<8;++i)check(before[i].handle==after[i].handle&&before[i].tag==after[i].tag&&before[i].vs==after[i].vs&&before[i].ps==after[i].ps,"fatal actual companion limit never recycles or relabels historical route indices");
    }
    driver=nullptr;
    std::cout<<"{\"schema\":\"dashboard-census-focused-v1\",\"checks\":"<<checks<<",\"startup_shader_events\":512,\"startup_route_events\":240,\"generic_capacity\":56,\"actual_companion_capacity\":8,\"actual_prepare_resources_pipeline\":true,\"manual_phase_collection\":true,\"fake_vulkan\":true,\"native_gpu_or_hardware_proven\":false}\n";
    return 0;
}
int run(){
    PhaseSequence phases;
    check(!phases.request(1),"cannot skip first closed phase");
    check(phases.request(0)&&phases.request(0),"same first phase is idempotent");
    check(!phases.request(2),"cannot skip dashboard-open phase");
    check(phases.request(1)&&phases.request(2)&&phases.request(3),"one closed/open/closed/collect transition");
    check(!phases.request(0),"spent diagnostic cannot rearm");
    Probe p;TestAccess::setup(p);
    p.beforeSubmit(h<VkQueue>(5),true);
    check(submits==0&&!TestAccess::disabledPublished(p),"initial disabled is not terminal collection and spends no phase3 fence");
    for(unsigned phase=0;phase<4;++phase){
        check(TestAccess::request(p,phase),"ordered request admitted");
        p.beforeSubmit(h<VkQueue>(5),true);
        check(submits==phase+1&&TestAccess::queued(p)==phase,"one distinct queue-ordered phase command");
        p.beforeSubmit(h<VkQueue>(5),true);
        check(submits==phase+1,"reused native submissions never reset or resubmit phase command");
    }
    check(TestAccess::disabledPublished(p),"collection fence published only after terminal submit");
    VkCommandBuffer command=h<VkCommandBuffer>(10);
    VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;allocate.commandPool=h<VkCommandPool>(9);allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
    p.commandsAllocated(allocate,&command,VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    TestAccess::seedOldRoute(p,command);p.begin(command,begin,VK_SUCCESS);
    check(TestAccess::cleanRoutes(p,command),"re-recorded command clears old route identities");
    TestAccess::seedOldRoute(p,command);p.reset(command,VK_SUCCESS);
    check(TestAccess::cleanRoutes(p,command),"successful command reset clears old routes");
    TestAccess::seedOldRoute(p,command);p.poolRetired(allocate.commandPool);
    check(TestAccess::cleanRoutes(p,command),"pool reset clears old routes");
    p.poolRetired(allocate.commandPool,true);
    check(TestAccess::absentCommand(p,command),"destroyed pool removes command handles before future handle reuse");
    p.commandsAllocated(allocate,&command,VK_SUCCESS);
    p.begin(command,begin,VK_SUCCESS);p.bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(11));
    p.nativeDescriptorBinding(command,VK_PIPELINE_BIND_POINT_GRAPHICS,1,1);
    check(!p.beforeDraw(command)&&operations.empty(),"native Set1 before scene stays untouched and refuses probe swap");
    p.begin(command,begin,VK_SUCCESS);p.bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(11));
    p.nativeDescriptorBinding(command,VK_PIPELINE_BIND_POINT_GRAPHICS,0,1);
    check(p.beforeDraw(command),"known successful original completion admits owned companion");
    operations.push_back(99);p.afterDraw(command);
    check(operations==std::vector<unsigned>({22,77,99,11}),"companion/set1/unchanged draw/normal pipeline restore order");
    operations.clear();p.unknown(command);
    check(!p.beforeDraw(command)&&operations.empty(),"query/secondary/unknown recording retains ordinary draw");
    p.begin(command,begin,VK_SUCCESS);p.bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(11));p.pipelineRetiring(h<VkPipeline>(11));
    check(!p.beforeDraw(command),"destroyed native pipeline cannot admit its historical companion");
    p.commandsFreed(allocate.commandPool,1,&command);
    check(!p.beforeDraw(command),"freed command handle cannot retain bound route");
    Probe secondary;TestAccess::setup(secondary);allocate.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    secondary.commandsAllocated(allocate,&command,VK_SUCCESS);secondary.begin(command,begin,VK_SUCCESS);secondary.bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(11));
    check(!secondary.beforeDraw(command),"secondary recordings never use new Set1");
    Probe unsupported;TestAccess::setup(unsupported);unsupported.beforeSubmit(h<VkQueue>(5),false);
    check(unsupported.refusal()==Refusal::Submit&&!TestAccess::disabledPublished(unsupported),"unsupported pre-capture submit stays normal and never claims empty collection");
    Probe queue;TestAccess::setup(queue);queue.beforeSubmit(h<VkQueue>(6),true);
    check(queue.refusal()==Refusal::Queue,"unknown queue never touches probe resources");
    Probe noFeature;TestAccess::setup(noFeature);frame_pixel_ssbo_clone::Recipe recipe;
    VkGraphicsPipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    noFeature.pipeline(h<VkPipeline>(77),VK_NULL_HANDLE,pipeline,recipe);
    check(noFeature.refusal()==Refusal::Feature,"missing actual feature proof refuses before any driver resource call");
    std::cout<<"{\"checks\":"<<checks<<",\"native_hardware_proven\":false,\"initial_terminal_and_reuse_contracts\":true,\"set0_and_restore_contracts\":true}\n";
    return 0;
}
}
int main(int argc,char** argv){try{
    if(argc==3&&!std::strcmp(argv[1],"--census-startup-only"))return dashboard_transition_probe::censusVectors(argv[2]);
    if(argc==3&&!std::strcmp(argv[1],"--queue-profile-only"))return dashboard_transition_probe::queueVectors(argv[2]);
    return dashboard_transition_probe::run();
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
