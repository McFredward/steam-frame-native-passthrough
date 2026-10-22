#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "stream_bridge_vulkan.cpp"
namespace {
unsigned checks=0;
void check(bool good,const char* message){++checks;if(!good){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
struct Dispatch {void* table;};
std::uintptr_t tables[128]{};unsigned nextTable=0;
template<class T>T dispatchable(void* table=nullptr){return reinterpret_cast<T>(new Dispatch{table?table:&tables[nextTable++]});}
template<class T>void destroyDispatch(T value){delete reinterpret_cast<Dispatch*>(value);}
template<class T>T scalar(std::uintptr_t value){return reinterpret_cast<T>(value);}
VkResult nativeResult=VK_SUCCESS;
unsigned nativeCreateInstances=0,nativeDestroyInstances=0,nativeCreateDevices=0,nativeDestroyDevices=0;
unsigned nativePools=0,nativePoolResets=0,nativeCbAllocs=0,nativeCbResets=0,nativeBegins=0,nativeFrees=0,nativeDraws=0;
unsigned nativeSubmits=0,nativeSubmits2=0,nativeSubmits2KHR=0,nativeShaderCreates=0;
std::vector<VkPipeline> binds;std::vector<std::uint32_t> sets;
std::uintptr_t nextPool=100;std::vector<VkCommandBuffer> allocatedHandles;
VkQueue seenQueue{};VkFence seenFence{};const void* seenSubmit=nullptr;std::uint32_t seenCount=0;
unsigned nativeDescriptorAlloc=0,nativeDescriptorFree=0,nativeDescriptorReset=0,nativeDescriptorDestroy=0,nativeDescriptorWrites=0,nativeDescriptorTemplates=0,nativeDescriptorTemplatesKHR=0;
VkDescriptorPool seenDescriptorPool{};const void* seenDescriptorInfo=nullptr;const void* seenDescriptorSets=nullptr;std::uint32_t seenDescriptorCount=0;VkDescriptorPoolResetFlags seenDescriptorFlags=0;
VkResult VKAPI_CALL nAllocateDescriptors(VkDevice,const VkDescriptorSetAllocateInfo* i,VkDescriptorSet* sets){++nativeDescriptorAlloc;seenDescriptorPool=i->descriptorPool;seenDescriptorInfo=i;seenDescriptorSets=sets;seenDescriptorCount=i->descriptorSetCount;if(nativeResult==VK_SUCCESS)for(unsigned n=0;n<i->descriptorSetCount;++n)sets[n]=scalar<VkDescriptorSet>(0x900+n);return nativeResult;}
VkResult VKAPI_CALL nFreeDescriptors(VkDevice,VkDescriptorPool p,std::uint32_t n,const VkDescriptorSet* sets){++nativeDescriptorFree;seenDescriptorPool=p;seenDescriptorSets=sets;seenDescriptorCount=n;return nativeResult;}
VkResult VKAPI_CALL nResetDescriptors(VkDevice,VkDescriptorPool p,VkDescriptorPoolResetFlags flags){++nativeDescriptorReset;seenDescriptorPool=p;seenDescriptorFlags=flags;return nativeResult;}
void VKAPI_CALL nDestroyDescriptors(VkDevice,VkDescriptorPool p,const VkAllocationCallbacks* a){++nativeDescriptorDestroy;seenDescriptorPool=p;seenDescriptorInfo=a;}
void VKAPI_CALL nUpdateDescriptors(VkDevice,std::uint32_t n,const VkWriteDescriptorSet* w,std::uint32_t copies,const VkCopyDescriptorSet* c){++nativeDescriptorWrites;seenDescriptorCount=n;seenDescriptorInfo=w;seenDescriptorSets=c;check(copies==1,"native descriptor copy count unchanged");}
void VKAPI_CALL nTemplate(VkDevice,VkDescriptorSet,VkDescriptorUpdateTemplate,const void*){++nativeDescriptorTemplates;}
void VKAPI_CALL nTemplateKHR(VkDevice,VkDescriptorSet,VkDescriptorUpdateTemplate,const void*){++nativeDescriptorTemplatesKHR;}
VkResult VKAPI_CALL nCreateInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance* out){++nativeCreateInstances;if(nativeResult==VK_SUCCESS)*out=dispatchable<VkInstance>();return nativeResult;}
void VKAPI_CALL nDestroyInstance(VkInstance value,const VkAllocationCallbacks*){++nativeDestroyInstances;destroyDispatch(value);}
VkResult VKAPI_CALL nCreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice* out){++nativeCreateDevices;if(nativeResult==VK_SUCCESS)*out=dispatchable<VkDevice>();return nativeResult;}
void VKAPI_CALL nDestroyDevice(VkDevice value,const VkAllocationCallbacks*){++nativeDestroyDevices;destroyDispatch(value);}
VkResult VKAPI_CALL nCreatePool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* out){++nativePools;if(nativeResult==VK_SUCCESS)*out=scalar<VkCommandPool>(++nextPool);return nativeResult;}
void VKAPI_CALL nDestroyPool(VkDevice,VkCommandPool,const VkAllocationCallbacks*){}
VkResult VKAPI_CALL nResetPool(VkDevice,VkCommandPool,VkCommandPoolResetFlags){++nativePoolResets;return nativeResult;}
VkResult VKAPI_CALL nAllocate(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){++nativeCbAllocs;if(nativeResult==VK_SUCCESS)for(unsigned n=0;n<info->commandBufferCount;++n){out[n]=dispatchable<VkCommandBuffer>(key(d));allocatedHandles.push_back(out[n]);}return nativeResult;}
void VKAPI_CALL nFree(VkDevice,VkCommandPool,unsigned,const VkCommandBuffer*){++nativeFrees;}
VkResult VKAPI_CALL nBegin(VkCommandBuffer,const VkCommandBufferBeginInfo*){++nativeBegins;return nativeResult;}
VkResult VKAPI_CALL nReset(VkCommandBuffer,VkCommandBufferResetFlags){++nativeCbResets;return nativeResult;}
void VKAPI_CALL nBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline p){binds.push_back(p);}
void VKAPI_CALL nBindSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,unsigned first,unsigned,const VkDescriptorSet*,unsigned,const unsigned*){sets.push_back(first);}
void VKAPI_CALL nDraw(VkCommandBuffer,unsigned,unsigned,unsigned,unsigned){++nativeDraws;}
void VKAPI_CALL nDrawIndexed(VkCommandBuffer,unsigned,unsigned,unsigned,int,unsigned){++nativeDraws;}
VkResult VKAPI_CALL nSubmit(VkQueue q,unsigned n,const VkSubmitInfo* info,VkFence f){++nativeSubmits;seenQueue=q;seenCount=n;seenSubmit=info;seenFence=f;return nativeResult;}
VkResult VKAPI_CALL nSubmit2(VkQueue q,unsigned n,const VkSubmitInfo2* info,VkFence f){++nativeSubmits2;seenQueue=q;seenCount=n;seenSubmit=info;seenFence=f;return nativeResult;}
VkResult VKAPI_CALL nSubmit2KHR(VkQueue q,unsigned n,const VkSubmitInfo2* info,VkFence f){++nativeSubmits2KHR;seenQueue=q;seenCount=n;seenSubmit=info;seenFence=f;return nativeResult;}
VkResult VKAPI_CALL nCreateShader(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule* out){++nativeShaderCreates;if(nativeResult==VK_SUCCESS)*out=scalar<VkShaderModule>(99);return nativeResult;}
void VKAPI_CALL marker(){}
PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice,const char* name){
#define FUN(n,f) if(!std::strcmp(name,"vk" #n))return reinterpret_cast<PFN_vkVoidFunction>(f);
 FUN(DestroyDevice,nDestroyDevice) FUN(CreateCommandPool,nCreatePool) FUN(DestroyCommandPool,nDestroyPool) FUN(ResetCommandPool,nResetPool)
 FUN(AllocateCommandBuffers,nAllocate) FUN(FreeCommandBuffers,nFree) FUN(BeginCommandBuffer,nBegin) FUN(ResetCommandBuffer,nReset)
 FUN(CmdBindPipeline,nBindPipeline) FUN(CmdBindDescriptorSets,nBindSets) FUN(CmdDraw,nDraw) FUN(CmdDrawIndexed,nDrawIndexed)
 FUN(QueueSubmit,nSubmit) FUN(QueueSubmit2,nSubmit2) FUN(QueueSubmit2KHR,nSubmit2KHR) FUN(CreateShaderModule,nCreateShader)
 FUN(AllocateDescriptorSets,nAllocateDescriptors) FUN(FreeDescriptorSets,nFreeDescriptors) FUN(ResetDescriptorPool,nResetDescriptors) FUN(DestroyDescriptorPool,nDestroyDescriptors)
 FUN(UpdateDescriptorSets,nUpdateDescriptors) FUN(UpdateDescriptorSetWithTemplate,nTemplate) FUN(UpdateDescriptorSetWithTemplateKHR,nTemplateKHR)
#undef FUN
 return !std::strcmp(name,"vkUnaffected")?marker:nullptr;
}
PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance,const char* name){
 if(!std::strcmp(name,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(nCreateInstance);
 if(!std::strcmp(name,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(nDestroyInstance);
 if(!std::strcmp(name,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(nCreateDevice);
 return !std::strcmp(name,"vkUnaffected")?marker:nullptr;
}
VkInstance makeInstance(){VkLayerInstanceLink chain{};chain.pfnNextGetInstanceProcAddr=gipa;
 VkLayerInstanceCreateInfo link{};link.sType=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;link.function=VK_LAYER_LINK_INFO;link.u.pLayerInfo=&chain;
 VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pNext=&link;VkInstance value{};
 check(createInstance(&info,nullptr,&value)==VK_SUCCESS,"native instance creation succeeds");check(link.u.pLayerInfo==nullptr,"instance loader link is advanced once");return value;}
VkDevice makeDevice(VkInstance i){VkLayerDeviceLink chain{};chain.pfnNextGetInstanceProcAddr=gipa;chain.pfnNextGetDeviceProcAddr=gdpa;
 VkLayerDeviceCreateInfo link{};link.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;link.function=VK_LAYER_LINK_INFO;link.u.pLayerInfo=&chain;
 VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};info.pNext=&link;VkDevice value{};
 auto physical=dispatchable<VkPhysicalDevice>(key(i));const auto result=createDevice(physical,&info,nullptr,&value);destroyDispatch(physical);
 check(result==VK_SUCCESS,"native device creation succeeds");check(link.u.pLayerInfo==nullptr,"device loader link is advanced once");return value;}
VkCommandPool makePool(VkDevice d,bool protectedMemory=false){VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};info.queueFamilyIndex=7;info.flags=protectedMemory?VK_COMMAND_POOL_CREATE_PROTECTED_BIT:0;VkCommandPool pool{};
 check(createCommandPool(d,&info,nullptr,&pool)==VK_SUCCESS,"pool forwards successful creation");return pool;}
VkCommandBuffer makeCommand(VkDevice d,VkCommandPool p){VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};info.commandPool=p;info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;info.commandBufferCount=1;VkCommandBuffer cb{};
 check(allocateCommandBuffers(d,&info,&cb)==VK_SUCCESS,"command allocation preserves native result");return cb;}
void descriptorLifetime(VkDevice d){
 auto& state=wrapper_fixture::state;const auto pool=scalar<VkDescriptorPool>(0x700),other=scalar<VkDescriptorPool>(0x701);VkDescriptorSetLayout layout=scalar<VkDescriptorSetLayout>(0x800);VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};info.descriptorPool=pool;info.descriptorSetCount=2;std::array<VkDescriptorSetLayout,2> layouts{{layout,layout}};info.pSetLayouts=layouts.data();std::array<VkDescriptorSet,300> sets{};
 check(framebridge_vkGetDeviceProcAddr(d,"vkAllocateDescriptorSets")==reinterpret_cast<PFN_vkVoidFunction>(allocateDescriptorSets),"descriptor allocation is observed through actual intercepted device dispatch");
 check(allocateDescriptorSets(d,&info,sets.data())==VK_SUCCESS&&nativeDescriptorAlloc==1&&seenDescriptorInfo==&info&&seenDescriptorSets==sets.data()&&seenDescriptorCount==2,"descriptor allocation forwards exact native pointers/count once");
 check(state.descriptorAllocations==1&&state.descriptorAllocatedPool==pool&&state.descriptorAllocatedCount==2&&state.descriptorAllocatedSets==sets.data(),"successful original allocation publishes actual owning pool and native handles");
 nativeResult=VK_ERROR_OUT_OF_POOL_MEMORY;check(allocateDescriptorSets(d,&info,sets.data())==nativeResult&&nativeDescriptorAlloc==2&&state.descriptorAllocations==1,"failed native allocation cannot create descriptor ownership");
 check(resetDescriptorPool(d,pool,3)==nativeResult&&nativeDescriptorReset==1&&seenDescriptorPool==pool&&seenDescriptorFlags==3&&!state.descriptorRetiredPool&&!state.descriptorGlobals,"failed native pool reset preserves prior descriptor authority");
 check(freeDescriptorSets(d,pool,2,sets.data())==nativeResult&&nativeDescriptorFree==1&&state.descriptorRetiredSets.empty(),"failed native descriptor free preserves still-live allocations");nativeResult=VK_SUCCESS;
 check(resetDescriptorPool(d,other,0)==VK_SUCCESS&&nativeDescriptorReset==2&&state.descriptorRetiredPool==other&&!state.descriptorGlobals,"successful native pool reset scopes observation to exact original pool");
 VkAllocationCallbacks callbacks{};destroyDescriptorPool(d,pool,&callbacks);check(nativeDescriptorDestroy==1&&seenDescriptorPool==pool&&seenDescriptorInfo==&callbacks&&state.descriptorRetiredPool==pool&&!state.descriptorGlobals,"native pool destroy preserves allocator and scopes retirement to that pool");
 for(unsigned n=0;n<sets.size();++n)sets[n]=scalar<VkDescriptorSet>(0x900+n);
 check(freeDescriptorSets(d,pool,sets.size(),sets.data())==VK_SUCCESS&&nativeDescriptorFree==2&&seenDescriptorCount==sets.size()&&seenDescriptorSets==sets.data()&&state.descriptorRetiredSets==std::vector<VkDescriptorSet>(sets.begin(),sets.end())&&!state.descriptorGlobals,"large successful native free retires exact sets without global invalidation");
 VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};copy.dstSet=sets[0];updateDescriptorSets(d,1,&write,1,&copy);check(nativeDescriptorWrites==1&&seenDescriptorCount==1&&seenDescriptorInfo==&write&&seenDescriptorSets==&copy&&state.descriptorUpdates==1,"native writes and copies retain exact public API transaction");
 const auto retired=state.descriptorRetiredSets.size();updateDescriptorSetWithTemplate(d,sets[0],scalar<VkDescriptorUpdateTemplate>(0xa00),&copy);updateDescriptorSetWithTemplateKHR(d,sets[1],scalar<VkDescriptorUpdateTemplate>(0xa01),&write);
 check(nativeDescriptorTemplates==1&&nativeDescriptorTemplatesKHR==1&&state.descriptorInvalidatedSets==std::vector<VkDescriptorSet>({sets[0],sets[1]})&&state.descriptorRetiredSets.size()==retired&&!state.descriptorGlobals,"core/KHR unknown templates invalidate only exact destination content, preserving allocation ownership");
}
}
int main(){
 using wrapper_fixture::state;
 VkNegotiateLayerInterface invalid{};check(framebridge_vkNegotiateLoaderLayerInterfaceVersion(&invalid)==VK_ERROR_INITIALIZATION_FAILED,"invalid loader version refused");
 VkNegotiateLayerInterface negotiation{};negotiation.sType=LAYER_NEGOTIATE_INTERFACE_STRUCT;negotiation.loaderLayerInterfaceVersion=3;
 check(framebridge_vkNegotiateLoaderLayerInterfaceVersion(&negotiation)==VK_SUCCESS&&negotiation.loaderLayerInterfaceVersion==2,"public loader interface negotiated");
 check(!framebridge_vkGetInstanceProcAddr(VK_NULL_HANDLE,nullptr),"null proc name unchanged");
 auto i=makeInstance();auto d=makeDevice(i);
 check(nativeCreateInstances==1&&nativeCreateDevices==1,"native creation called exactly once");
 check(state.prepared==1,"minimum renderer forwarding prepared even without optional memory query");
 check(framebridge_vkGetDeviceProcAddr(d,"vkUnaffected")==marker,"unknown function forwarded untouched");
 descriptorLifetime(d);
 check(!framebridge_vkGetDeviceProcAddr(d,"vkCmdBindShadersEXT"),"absent extension stays absent");
 check(physicalProc(i,"vkUnaffected")==marker,"optional physical dispatch falls back to original instance proc");
 state.enabled=false;check(framebridge_vkGetDeviceProcAddr(d,"vkCmdDraw")==reinterpret_cast<PFN_vkVoidFunction>(nDraw),"disabled instrumentation keeps original proc");state.enabled=true;
 auto p=makePool(d);auto cb=makeCommand(d,p);check(state.allocated==1&&state.family==7&&state.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY,"allocation preserves level and pool queue family");
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 check(beginCommandBuffer(cb,&begin)==VK_SUCCESS&&state.begins==1&&nativeBegins==1,"begin observed after native success");
 const auto retiredBefore=state.retired;
 check(resetCommandBuffer(cb,0)==VK_SUCCESS&&state.resets==1&&state.retired==retiredBefore,"CB reset preserves allocation metadata");
 check(beginCommandBuffer(cb,&begin)==VK_SUCCESS&&state.begins==2,"same allocation can record after CB reset");
 check(resetCommandPool(d,p,0)==VK_SUCCESS&&state.resets==2&&state.retired==retiredBefore,"pool reset preserves allocations");
 check(beginCommandBuffer(cb,&begin)==VK_SUCCESS&&state.begins==3,"same allocation can record after pool reset");
 nativeResult=VK_ERROR_DEVICE_LOST;const auto beginsBefore=state.begins;
 check(beginCommandBuffer(cb,&begin)==nativeResult&&state.begins==beginsBefore&&state.unknown==1,"failed native begin cannot authorize recording");
 check(resetCommandBuffer(cb,0)==nativeResult&&state.unknown==2,"failed reset preserves native error and invalidates recording");nativeResult=VK_SUCCESS;
 auto protectedPool=makePool(d,true);const auto allocatedBefore=state.allocated;auto protectedCb=makeCommand(d,protectedPool);
 check(state.allocated==allocatedBefore,"protected pool is never renderer eligible");
 sbg::fixtureDraw={scalar<VkPipeline>(10),scalar<VkPipeline>(20),scalar<VkPipelineLayout>(30),scalar<VkDescriptorSet>(40),scalar<VkDescriptorSet>(50),true};
 cmdDraw(cb,3,1,0,0);check(nativeDraws==1&&binds.size()==2&&binds[0]==scalar<VkPipeline>(20)&&binds[1]==scalar<VkPipeline>(10),"draw binds replacement and restores native pipeline");
 check(sets==std::vector<unsigned>({1,2}),"draw binds only owned coverage and control sets");
 const auto protectedBindsBefore=binds.size();cmdDraw(protectedCb,3,1,0,0);check(nativeDraws==2&&binds.size()==protectedBindsBefore,"protected command forwards without replacement");
 const auto coverageDraw=sbg::fixtureDraw;const auto priorSets=sets.size();sbg::fixtureDraw.layout={};sbg::fixtureDraw.coverage={};sbg::fixtureDraw.control={};
 cmdDraw(cb,3,1,0,0);check(nativeDraws==3&&binds.size()==protectedBindsBefore+2&&sets.size()==priorSets,"legacy companion changes only the pipeline");sbg::fixtureDraw=coverageDraw;const auto bindsBefore=binds.size();
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;
 auto q=dispatchable<VkQueue>(key(d));auto f=scalar<VkFence>(77);nativeResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
 check(queueSubmit(q,1,&submit,f)==nativeResult&&nativeSubmits==1&&seenQueue==q&&seenFence==f&&seenSubmit==&submit&&seenCount==1,"submit preserves native args fence result and single call");
 VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
 check(queueSubmit2(q,1,&submit2,f)==nativeResult&&nativeSubmits2==1&&seenSubmit==&submit2,"core submit2 forwards once");
 check(queueSubmit2KHR(q,1,&submit2,f)==nativeResult&&nativeSubmits2KHR==1,"KHR submit2 preserves separate original entry");nativeResult=VK_SUCCESS;destroyDispatch(q);
 const auto lostBefore=state.lost;sbg::fixtureDraw.control=VK_NULL_HANDLE;cmdDrawIndexed(cb,3,1,0,0,0);
 check(state.lost==lostBefore+1&&nativeDraws==4&&binds.size()==bindsBefore,"incomplete optional draw stays wholly native");
 destroyCommandPool(d,protectedPool,nullptr);destroyCommandPool(d,p,nullptr);
 auto stale=key(d);destroyDevice(d,nullptr);check(nativeDestroyDevices==1,"destroy forwards once with dispatch freed by driver");
 check(!lookup<Device>(registry().devices,stale),"destroy removes registry by cached dispatch key");
 // A fresh device exercises observer failure without affecting the native call.
 d=makeDevice(i);state.throwJournal=true;VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};VkShaderModule module{};
 check(createShaderModule(d,&shader,nullptr,&module)==VK_SUCCESS&&nativeShaderCreates==1&&module==scalar<VkShaderModule>(99),"observer exception cannot alter successful native creation");
 check(state.lost==lostBefore+2,"observer exception closes optional route");state.throwJournal=false;destroyDevice(d,nullptr);
 // Inventories are finite: pressure closes only the optional route.
 d=makeDevice(i);const auto priorLost=state.lost;std::array<VkCommandPool,129> pools{};
 for(auto& pool:pools)pool=makePool(d);
 check(state.lost==priorLost+1,"pool inventory pressure refuses optional renderer without evicting live facts");destroyDevice(d,nullptr);
 // Failure before the native call is a legal Vulkan host-memory refusal.
 state.throwConstructor=true;VkLayerDeviceLink chain{};chain.pfnNextGetInstanceProcAddr=gipa;chain.pfnNextGetDeviceProcAddr=gdpa;
 VkLayerDeviceCreateInfo link{};link.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;link.function=VK_LAYER_LINK_INFO;link.u.pLayerInfo=&chain;
 VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.pNext=&link;VkDevice unused{};
 auto physical=dispatchable<VkPhysicalDevice>(key(i));const auto createsBefore=nativeCreateDevices;
 check(createDevice(physical,&deviceInfo,nullptr,&unused)==VK_ERROR_OUT_OF_HOST_MEMORY&&nativeCreateDevices==createsBefore,"owned constructor exception cannot escape the Vulkan ABI or create an orphan device");state.throwConstructor=false;destroyDispatch(physical);
 // Recovery intentionally has no mask set. It must not poison the device
 // before the real game has supplied its first exact image.
 d=makeDevice(i);p=makePool(d);cb=makeCommand(d,p);check(beginCommandBuffer(cb,&begin)==VK_SUCCESS,"camera-only wrapper control begins a real admitted command");
 const auto recoveryLost=state.lost,recoveryDraws=nativeDraws;const auto recoverySets=sets.size(),recoveryBinds=binds.size();
 sbg::fixtureDraw={scalar<VkPipeline>(10),scalar<VkPipeline>(21),scalar<VkPipelineLayout>(30),VK_NULL_HANDLE,scalar<VkDescriptorSet>(50),true};
 cmdDraw(cb,3,1,0,0);cmdDrawIndexed(cb,3,1,0,0,0);
 check(state.lost==recoveryLost&&nativeDraws==recoveryDraws+2,"camera-only companion without mask preserves device tracking and forwards both native draw forms");
 check(binds.size()==recoveryBinds+4&&binds[recoveryBinds]==scalar<VkPipeline>(21)&&binds[recoveryBinds+1]==scalar<VkPipeline>(10)&&binds[recoveryBinds+2]==scalar<VkPipeline>(21)&&binds[recoveryBinds+3]==scalar<VkPipeline>(10),"camera-only draw restores the exact original pipeline for both draw forms");
 check(sets.size()==recoverySets+2&&sets[recoverySets]==2&&sets[recoverySets+1]==2,"camera-only draw binds control set2 and never an absent mask set1");
 sbg::fixtureDraw=coverageDraw;cmdDraw(cb,3,1,0,0);
 check(state.lost==recoveryLost&&nativeDraws==recoveryDraws+3&&sets[sets.size()-2]==1&&sets.back()==2,"fresh exact game data resumes normal mask/control binding after camera-only recovery");
 destroyCommandPool(d,p,nullptr);destroyDevice(d,nullptr);
 auto instanceKey=key(i);destroyInstance(i,nullptr);check(nativeDestroyInstances==1&&!lookup<Instance>(registry().instances,instanceKey),"instance destroy never dereferences freed native handle");
 for(auto handle:allocatedHandles)destroyDispatch(handle);
 std::printf("%u Vulkan wrapper checks passed\n",checks);
}
