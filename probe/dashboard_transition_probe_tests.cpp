#include "dashboard_transition_probe.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace dashboard_transition_probe {
namespace {
unsigned checks=0,submits=0;
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
};
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
int main(){try{return dashboard_transition_probe::run();}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
