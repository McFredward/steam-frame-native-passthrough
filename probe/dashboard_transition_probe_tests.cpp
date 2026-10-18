#include "dashboard_transition_probe.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <limits>
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
    ~PrivateRun(){if(fd>=0){for(const char* name:{"dashboard-transition.enable","dashboard-transition.spv","dashboard-transition-queue-profile.json","dashboard-transition-status-00.json"})unlinkat(fd,name,0);close(fd);}if(!directory.empty())rmdir(directory.c_str());}
};
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
    if(argc==3&&!std::strcmp(argv[1],"--queue-profile-only"))return dashboard_transition_probe::queueVectors(argv[2]);
    return dashboard_transition_probe::run();
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
