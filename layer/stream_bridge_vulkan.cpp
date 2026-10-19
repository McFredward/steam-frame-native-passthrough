// Optional full-colour stream bridge. Every native call is forwarded once;
// selected drawing is changed only by a current, fully prepared renderer lease.
#include "stream_bridge_renderer.h"
#include <vulkan/vk_layer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#define SFNP_EXPORT extern "C" __attribute__((visibility("default")))
namespace sbg=stream_bridge_renderer;
SFNP_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framebridge_vkGetInstanceProcAddr(VkInstance,const char*);
SFNP_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framebridge_vkGetDeviceProcAddr(VkDevice,const char*);
namespace {
template<class T>void* key(T value){return value?*reinterpret_cast<void**>(value):nullptr;}
struct Instance{VkInstance native{};PFN_vkGetInstanceProcAddr gipa{};PFN_GetPhysicalDeviceProcAddr physical{};uint32_t api=0;};
struct Device{
    VkDevice native{};PFN_vkGetDeviceProcAddr gdpa{};sbg::Renderer renderer;
#define FIELD(n) PFN_vk##n n=nullptr;
    FIELD(DeviceWaitIdle) FIELD(DestroyDevice) FIELD(CreateShaderModule) FIELD(DestroyShaderModule)
    FIELD(CreateDescriptorSetLayout) FIELD(DestroyDescriptorSetLayout) FIELD(CreatePipelineLayout) FIELD(DestroyPipelineLayout)
    FIELD(CreateRenderPass) FIELD(CreateRenderPass2) FIELD(CreateRenderPass2KHR) FIELD(DestroyRenderPass)
    FIELD(CreateGraphicsPipelines) FIELD(DestroyPipeline)
    FIELD(CreateImage) FIELD(DestroyImage) FIELD(AllocateMemory) FIELD(FreeMemory)
    FIELD(BindImageMemory) FIELD(BindImageMemory2) FIELD(BindImageMemory2KHR) FIELD(GetMemoryFdKHR)
    FIELD(CreateImageView) FIELD(DestroyImageView) FIELD(UpdateDescriptorSets) FIELD(UpdateDescriptorSetWithTemplate)
    FIELD(UpdateDescriptorSetWithTemplateKHR) FIELD(AllocateDescriptorSets) FIELD(FreeDescriptorSets) FIELD(ResetDescriptorPool) FIELD(DestroyDescriptorPool)
    FIELD(GetDeviceQueue) FIELD(GetDeviceQueue2) FIELD(CreateCommandPool) FIELD(DestroyCommandPool) FIELD(ResetCommandPool)
    FIELD(AllocateCommandBuffers) FIELD(FreeCommandBuffers) FIELD(BeginCommandBuffer) FIELD(ResetCommandBuffer)
    FIELD(CmdBindPipeline) FIELD(CmdBindDescriptorSets) FIELD(CmdDraw) FIELD(CmdDrawIndexed)
    FIELD(CmdExecuteCommands) FIELD(CmdBindShadersEXT) FIELD(CmdPushDescriptorSetKHR)
    FIELD(QueueSubmit) FIELD(QueueSubmit2) FIELD(QueueSubmit2KHR)
#undef FIELD
    struct Pool {VkCommandPool value{};uint32_t family=UINT32_MAX;bool protectedMemory=false;};
    struct Command {VkCommandBuffer value{};VkCommandPool pool{};};
    std::mutex poolMutex;
    std::array<Pool,128> pools{};
    std::array<Command,1024> commands{};
    std::atomic<bool> tracking=true;
};
template<class T>struct Owned {void* dispatch=nullptr;std::unique_ptr<T> value;};
struct Registry {
    std::mutex mutex;
    std::array<Owned<Instance>,16> instances{};
    std::array<Owned<Device>,32> devices{};
};
Registry& registry(){static auto* value=new Registry;return *value;}
template<class T,class C>T* lookup(C& values,void* dispatch) noexcept {for(auto& entry:values)if(entry.value&&entry.dispatch==dispatch)return entry.value.get();return nullptr;}
template<class T,std::size_t N>bool retain(std::array<Owned<T>,N>& values,void* dispatch,std::unique_ptr<T>& value) noexcept {
    if(lookup<T>(values,dispatch))return false;
    for(auto& entry:values)if(!entry.value){entry.dispatch=dispatch;entry.value=std::move(value);return true;}
    return false;
}
template<class T,std::size_t N>std::unique_ptr<T> remove(std::array<Owned<T>,N>& values,void* dispatch) noexcept {
    for(auto& entry:values)if(entry.value&&entry.dispatch==dispatch){entry.dispatch=nullptr;return std::move(entry.value);}
    return {};
}
template<class T>Instance* instance(T object){if(!object)return nullptr;auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);return lookup<Instance>(r.instances,key(object));}
template<class T>Device* device(T object){if(!object)return nullptr;auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);return lookup<Device>(r.devices,key(object));}
// Fixed inventories never allocate on command or draw callbacks. Loss of even
// one lifecycle observation makes the optional renderer refuse the whole device.
void lost(Device& d) noexcept {d.tracking.store(false,std::memory_order_release);d.renderer.trackingLost();}
template<class F>void observe(Device& d,F&& callback) noexcept {
    if(!d.tracking.load(std::memory_order_acquire))return;
    try{callback();}catch(...){lost(d);}
}
VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* alloc,VkInstance* out){
    if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;
    auto* link=static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while(link&&(link->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO||link->function!=VK_LAYER_LINK_INFO))link=static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if(!link||!link->u.pLayerInfo||!link->u.pLayerInfo->pfnNextGetInstanceProcAddr)return VK_ERROR_INITIALIZATION_FAILED;
    const auto gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next=reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE,"vkCreateInstance"));
    if(!next)return VK_ERROR_INITIALIZATION_FAILED;
    try{
        auto i=std::make_unique<Instance>();auto& r=registry();
        i->gipa=gipa;i->physical=link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
        link->u.pLayerInfo=link->u.pLayerInfo->pNext;
        const auto result=next(info,alloc,out);if(result!=VK_SUCCESS)return result;
        const auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out,"vkDestroyInstance"));
        struct Cleanup {VkInstance* out;PFN_vkDestroyInstance destroy;const VkAllocationCallbacks* alloc;bool active=true;
            ~Cleanup(){if(active){if(destroy)destroy(*out,alloc);*out=VK_NULL_HANDLE;}}} cleanup{out,destroy,alloc};
        i->native=*out;i->api=info->pApplicationInfo&&info->pApplicationInfo->apiVersion?info->pApplicationInfo->apiVersion:VK_API_VERSION_1_0;
        bool retained=false;{std::lock_guard<std::mutex> lock(r.mutex);retained=retain(r.instances,key(*out),i);}
        if(retained){cleanup.active=false;return result;}
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance value,const VkAllocationCallbacks* alloc){
    if(!value)return;
    auto* i=instance(value);if(!i)return;
    const auto dispatch=key(value);const auto next=reinterpret_cast<PFN_vkDestroyInstance>(i->gipa(value,"vkDestroyInstance"));
    if(next)next(value,alloc);
    // The driver may have freed the dispatchable handle. Use only the cached key.
    std::unique_ptr<Instance> owned;{auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);owned=remove(r.instances,dispatch);}
}
VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* out){
    if(!info||!out)return VK_ERROR_INITIALIZATION_FAILED;
    auto* i=instance(physical);auto* link=static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while(link&&(link->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO||link->function!=VK_LAYER_LINK_INFO))link=static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    if(!i||!link||!link->u.pLayerInfo||!link->u.pLayerInfo->pfnNextGetInstanceProcAddr)return VK_ERROR_INITIALIZATION_FAILED;
    try{
    auto d=std::make_unique<Device>();auto& r=registry();d->gdpa=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto next=reinterpret_cast<PFN_vkCreateDevice>(link->u.pLayerInfo->pfnNextGetInstanceProcAddr(i->native,"vkCreateDevice"));link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    if(!next||!d->gdpa)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next(physical,info,alloc,out);if(result!=VK_SUCCESS)return result;d->native=*out;
    d->DestroyDevice=reinterpret_cast<PFN_vkDestroyDevice>(d->gdpa(*out,"vkDestroyDevice"));
    struct Cleanup {Device* device;VkDevice* out;const VkAllocationCallbacks* alloc;bool active=true;
        ~Cleanup(){if(active){device->renderer.stopCold();if(device->DestroyDevice)device->DestroyDevice(*out,alloc);*out=VK_NULL_HANDLE;}}} cleanup{d.get(),out,alloc};
#define LOAD(n) d->n=reinterpret_cast<PFN_vk##n>(d->gdpa(*out,"vk" #n));
    LOAD(DeviceWaitIdle) LOAD(DestroyDevice) LOAD(CreateShaderModule) LOAD(DestroyShaderModule)
    LOAD(CreateDescriptorSetLayout) LOAD(DestroyDescriptorSetLayout) LOAD(CreatePipelineLayout) LOAD(DestroyPipelineLayout)
    LOAD(CreateRenderPass) LOAD(CreateRenderPass2) LOAD(CreateRenderPass2KHR) LOAD(DestroyRenderPass)
    LOAD(CreateGraphicsPipelines) LOAD(DestroyPipeline) LOAD(CreateImage) LOAD(DestroyImage) LOAD(AllocateMemory) LOAD(FreeMemory)
    LOAD(BindImageMemory) LOAD(BindImageMemory2) LOAD(BindImageMemory2KHR) LOAD(GetMemoryFdKHR)
    LOAD(CreateImageView) LOAD(DestroyImageView) LOAD(UpdateDescriptorSets) LOAD(UpdateDescriptorSetWithTemplate)
    LOAD(UpdateDescriptorSetWithTemplateKHR) LOAD(AllocateDescriptorSets) LOAD(FreeDescriptorSets) LOAD(ResetDescriptorPool) LOAD(DestroyDescriptorPool)
    LOAD(GetDeviceQueue) LOAD(GetDeviceQueue2) LOAD(CreateCommandPool) LOAD(DestroyCommandPool) LOAD(ResetCommandPool)
    LOAD(AllocateCommandBuffers) LOAD(FreeCommandBuffers) LOAD(BeginCommandBuffer) LOAD(ResetCommandBuffer)
    LOAD(CmdBindPipeline) LOAD(CmdBindDescriptorSets) LOAD(CmdDraw) LOAD(CmdDrawIndexed) LOAD(CmdExecuteCommands) LOAD(CmdBindShadersEXT) LOAD(CmdPushDescriptorSetKHR)
    LOAD(QueueSubmit) LOAD(QueueSubmit2) LOAD(QueueSubmit2KHR)
#undef LOAD
    VkPhysicalDeviceMemoryProperties memory{};uint32_t family=UINT32_MAX;const auto getMemory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(i->gipa(i->native,"vkGetPhysicalDeviceMemoryProperties"));
    if(sbg::enabled()&&getMemory){getMemory(physical,&memory);uint32_t count=0;const auto getQueues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(i->gipa(i->native,"vkGetPhysicalDeviceQueueFamilyProperties"));
        if(getQueues){getQueues(physical,&count,nullptr);if(count<=64){VkQueueFamilyProperties queues[64]{};getQueues(physical,&count,queues);if(count>64)count=0;for(uint32_t n=0;n<info->queueCreateInfoCount&&n<64;++n){const auto& q=info->pQueueCreateInfos[n];if(!q.flags&&q.queueCount&&q.queueFamilyIndex<count&&(queues[q.queueFamilyIndex].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=q.queueFamilyIndex;break;}}}}
    }
    d->renderer.prepareCold(*out,memory,d->gdpa,*info,i->api,family);
    bool retained=false;{std::lock_guard<std::mutex> lock(r.mutex);retained=retain(r.devices,key(*out),d);}
    if(retained){cleanup.active=false;return result;}
    return VK_ERROR_OUT_OF_HOST_MEMORY;
    }catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice value,const VkAllocationCallbacks* alloc){
    if(!value)return;
    auto* d=device(value);if(!d)return;const auto dispatch=key(value);const auto next=d->DestroyDevice;
    d->renderer.stopCold();if(next)next(value,alloc);
    std::unique_ptr<Device> owned;{auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);owned=remove(r.devices,dispatch);}
}
VKAPI_ATTR VkResult VKAPI_CALL createShaderModule(VkDevice value,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* alloc,VkShaderModule* out){auto* d=device(value);const auto result=d->CreateShaderModule(value,info,alloc,out);if(result==VK_SUCCESS)observe(*d,[&]{d->renderer.shaderCreated(*out,*info);});return result;}
VKAPI_ATTR void VKAPI_CALL destroyShaderModule(VkDevice value,VkShaderModule object,const VkAllocationCallbacks* alloc){auto* d=device(value);observe(*d,[&]{d->renderer.definitions().destroyShader(object);});d->DestroyShaderModule(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL createDescriptorSetLayout(VkDevice value,const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks* alloc,VkDescriptorSetLayout* out){auto* d=device(value);const auto result=d->CreateDescriptorSetLayout(value,info,alloc,out);if(result==VK_SUCCESS)observe(*d,[&]{d->renderer.definitions().createSet(*out,*info,result);});return result;}
VKAPI_ATTR void VKAPI_CALL destroyDescriptorSetLayout(VkDevice value,VkDescriptorSetLayout object,const VkAllocationCallbacks* alloc){auto* d=device(value);observe(*d,[&]{d->renderer.definitions().destroySet(object);});d->DestroyDescriptorSetLayout(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL createPipelineLayout(VkDevice value,const VkPipelineLayoutCreateInfo* info,const VkAllocationCallbacks* alloc,VkPipelineLayout* out){auto* d=device(value);const auto result=d->CreatePipelineLayout(value,info,alloc,out);if(result==VK_SUCCESS)observe(*d,[&]{d->renderer.definitions().createLayout(*out,*info,result);});return result;}
VKAPI_ATTR void VKAPI_CALL destroyPipelineLayout(VkDevice value,VkPipelineLayout object,const VkAllocationCallbacks* alloc){auto* d=device(value);observe(*d,[&]{d->renderer.definitions().destroyLayout(object);});d->DestroyPipelineLayout(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL createRenderPass(VkDevice value,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* alloc,VkRenderPass* out){auto* d=device(value);const auto result=d->CreateRenderPass(value,info,alloc,out);if(result==VK_SUCCESS)observe(*d,[&]{d->renderer.definitions().createRenderPass(*out,*info,result);});return result;}
VKAPI_ATTR void VKAPI_CALL destroyRenderPass(VkDevice value,VkRenderPass object,const VkAllocationCallbacks* alloc){auto* d=device(value);observe(*d,[&]{d->renderer.definitions().destroyRenderPass(object);});d->DestroyRenderPass(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL createRenderPass2(VkDevice value,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* alloc,VkRenderPass* out){auto* d=device(value);const auto result=d->CreateRenderPass2(value,info,alloc,out);if(result==VK_SUCCESS)observe(*d,[&]{d->renderer.definitions().createRenderPass2(*out,*info,result);});return result;}
VKAPI_ATTR VkResult VKAPI_CALL createRenderPass2KHR(VkDevice value,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* alloc,VkRenderPass* out){auto* d=device(value);const auto result=d->CreateRenderPass2KHR(value,info,alloc,out);if(result==VK_SUCCESS)observe(*d,[&]{d->renderer.definitions().createRenderPass2(*out,*info,result);});return result;}
VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(VkDevice value,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* info,const VkAllocationCallbacks* alloc,VkPipeline* out){auto* d=device(value);const auto result=d->CreateGraphicsPipelines(value,cache,count,info,alloc,out);if(result==VK_SUCCESS&&count<=256)for(uint32_t n=0;n<count;++n)d->renderer.graphicsCreated(cache,info[n],alloc,out[n]);return result;}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice value,VkPipeline object,const VkAllocationCallbacks* alloc){auto* d=device(value);d->renderer.pipelineDestroyed(object);d->DestroyPipeline(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice value,const VkImageCreateInfo* info,const VkAllocationCallbacks* alloc,VkImage* out){auto* d=device(value);const auto result=d->CreateImage(value,info,alloc,out);if(result==VK_SUCCESS)d->renderer.imageCreated(*out,*info);return result;}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice value,VkImage object,const VkAllocationCallbacks* alloc){auto* d=device(value);d->renderer.imageDestroyed(object);d->DestroyImage(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice value,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* alloc,VkDeviceMemory* out){auto* d=device(value);auto preparation=d->renderer.memoryPreparing(*info);const auto result=d->AllocateMemory(value,info,alloc,out);d->renderer.memoryAllocated(result==VK_SUCCESS?*out:VK_NULL_HANDLE,*info,preparation,result);return result;}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice value,VkDeviceMemory object,const VkAllocationCallbacks* alloc){auto* d=device(value);d->renderer.memoryFreed(object);d->FreeMemory(value,object,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory(VkDevice value,VkImage image,VkDeviceMemory memory,VkDeviceSize offset){auto* d=device(value);const auto result=d->BindImageMemory(value,image,memory,offset);if(result==VK_SUCCESS)d->renderer.imageBound(image,memory,offset);return result;}
VkResult bindImageMemory2Impl(VkDevice value,uint32_t count,const VkBindImageMemoryInfo* info,PFN_vkBindImageMemory2 next){auto* d=device(value);const auto result=next(value,count,info);if(result==VK_SUCCESS&&count<=256)for(uint32_t n=0;n<count;++n)d->renderer.imageBound(info[n].image,info[n].memory,info[n].memoryOffset);return result;}
VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory2(VkDevice value,uint32_t count,const VkBindImageMemoryInfo* info){return bindImageMemory2Impl(value,count,info,device(value)->BindImageMemory2);}
VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory2KHR(VkDevice value,uint32_t count,const VkBindImageMemoryInfo* info){return bindImageMemory2Impl(value,count,info,device(value)->BindImageMemory2KHR);}
VKAPI_ATTR VkResult VKAPI_CALL getMemoryFdKHR(VkDevice value,const VkMemoryGetFdInfoKHR* info,int* fd){auto* d=device(value);const auto result=d->GetMemoryFdKHR(value,info,fd);if(result==VK_SUCCESS)d->renderer.memoryExported(info->memory,*fd,info->handleType,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));return result;}
VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice value,const VkImageViewCreateInfo* info,const VkAllocationCallbacks* alloc,VkImageView* out){auto* d=device(value);const auto result=d->CreateImageView(value,info,alloc,out);if(result==VK_SUCCESS)d->renderer.viewCreated(*out,*info);return result;}
VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice value,VkImageView object,const VkAllocationCallbacks* alloc){auto* d=device(value);d->renderer.viewDestroyed(object);d->DestroyImageView(value,object,alloc);}
VKAPI_ATTR void VKAPI_CALL updateDescriptorSets(VkDevice value,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet* copy){auto* d=device(value);d->renderer.descriptorWrites(count,writes,copies,copy);d->UpdateDescriptorSets(value,count,writes,copies,copy);}
VKAPI_ATTR void VKAPI_CALL updateDescriptorSetWithTemplate(VkDevice value,VkDescriptorSet set,VkDescriptorUpdateTemplate templ,const void* data){auto* d=device(value);d->renderer.descriptorInvalidated(set);d->UpdateDescriptorSetWithTemplate(value,set,templ,data);}
VKAPI_ATTR void VKAPI_CALL updateDescriptorSetWithTemplateKHR(VkDevice value,VkDescriptorSet set,VkDescriptorUpdateTemplate templ,const void* data){auto* d=device(value);d->renderer.descriptorInvalidated(set);d->UpdateDescriptorSetWithTemplateKHR(value,set,templ,data);}
VKAPI_ATTR VkResult VKAPI_CALL allocateDescriptorSets(VkDevice value,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* sets){auto* d=device(value);const auto result=d->AllocateDescriptorSets(value,info,sets);if(result==VK_SUCCESS&&info&&sets)d->renderer.descriptorsAllocated(*info,sets);return result;}
VKAPI_ATTR VkResult VKAPI_CALL freeDescriptorSets(VkDevice value,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets){auto* d=device(value);const auto result=d->FreeDescriptorSets(value,pool,count,sets);if(result==VK_SUCCESS){if(sets)for(std::size_t first=0;first<count;first+=256)for(std::size_t n=first;n<std::min<std::size_t>(count,first+256);++n)d->renderer.descriptorRetired(sets[n]);else if(count)d->renderer.descriptorsInvalidated();}return result;}
VKAPI_ATTR VkResult VKAPI_CALL resetDescriptorPool(VkDevice value,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags){auto* d=device(value);const auto result=d->ResetDescriptorPool(value,pool,flags);if(result==VK_SUCCESS)d->renderer.descriptorPoolRetired(pool);return result;}
VKAPI_ATTR void VKAPI_CALL destroyDescriptorPool(VkDevice value,VkDescriptorPool pool,const VkAllocationCallbacks* alloc){auto* d=device(value);d->renderer.descriptorPoolRetired(pool);d->DestroyDescriptorPool(value,pool,alloc);}
VKAPI_ATTR void VKAPI_CALL getDeviceQueue(VkDevice value,uint32_t family,uint32_t index,VkQueue* out){auto* d=device(value);d->GetDeviceQueue(value,family,index,out);d->renderer.queueObserved(*out,family);}
VKAPI_ATTR void VKAPI_CALL getDeviceQueue2(VkDevice value,const VkDeviceQueueInfo2* info,VkQueue* out){auto* d=device(value);d->GetDeviceQueue2(value,info,out);if(!info->flags)d->renderer.queueObserved(*out,info->queueFamilyIndex);}
VKAPI_ATTR VkResult VKAPI_CALL createCommandPool(VkDevice value,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* alloc,VkCommandPool* out){
    auto* d=device(value);const auto result=d->CreateCommandPool(value,info,alloc,out);
    if(result==VK_SUCCESS)observe(*d,[&]{std::lock_guard<std::mutex> lock(d->poolMutex);
        for(const auto& p:d->pools)if(p.value==*out){lost(*d);return;}
        for(auto& p:d->pools)if(!p.value){p={*out,info->queueFamilyIndex,(info->flags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT)!=0};return;}
        lost(*d);
    });return result;
}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice value,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){
    auto* d=device(value);const auto result=d->AllocateCommandBuffers(value,info,out);
    if(result==VK_SUCCESS)observe(*d,[&]{std::lock_guard<std::mutex> lock(d->poolMutex);
        if(info->commandBufferCount>d->commands.size()){lost(*d);return;}
        const Device::Pool* pool=nullptr;for(const auto& p:d->pools)if(p.value==info->commandPool){pool=&p;break;}
        for(uint32_t n=0;n<info->commandBufferCount;++n){
            Device::Command* vacant=nullptr;for(auto& c:d->commands){if(c.value==out[n]){lost(*d);return;}if(!c.value&&!vacant)vacant=&c;}
            if(!vacant){lost(*d);return;}*vacant={out[n],info->commandPool};
            if(pool&&!pool->protectedMemory)d->renderer.commandAllocated(out[n],info->level,pool->family);
        }
    });return result;
}
VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice value,VkCommandPool pool,uint32_t count,const VkCommandBuffer* buffers){
    auto* d=device(value);observe(*d,[&]{std::lock_guard<std::mutex> lock(d->poolMutex);
        if(count>d->commands.size()){lost(*d);return;}
        for(uint32_t n=0;n<count;++n){d->renderer.commandRetired(buffers[n]);for(auto& c:d->commands)if(c.value==buffers[n])c={};}
    });d->FreeCommandBuffers(value,pool,count,buffers);
}
void retirePool(Device& d,VkCommandPool pool,bool erase) noexcept {
    observe(d,[&]{std::lock_guard<std::mutex> lock(d.poolMutex);
        for(auto& c:d.commands)if(c.value&&c.pool==pool){
            if(erase){d.renderer.commandRetired(c.value);c={};}else d.renderer.commandReset(c.value);
        }
        if(erase)for(auto& p:d.pools)if(p.value==pool)p={};
    });
}
VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice value,VkCommandPool pool,const VkAllocationCallbacks* alloc){auto* d=device(value);retirePool(*d,pool,true);d->DestroyCommandPool(value,pool,alloc);}
VKAPI_ATTR VkResult VKAPI_CALL resetCommandPool(VkDevice value,VkCommandPool pool,VkCommandPoolResetFlags flags){
    auto* d=device(value);const auto result=d->ResetCommandPool(value,pool,flags);if(result==VK_SUCCESS)retirePool(*d,pool,false);else lost(*d);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer command,const VkCommandBufferBeginInfo* info){
    auto* d=device(command);const auto result=d->BeginCommandBuffer(command,info);
    if(result==VK_SUCCESS&&d->tracking.load(std::memory_order_acquire))d->renderer.commandBegin(command,info);else d->renderer.commandUnknown(command);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL resetCommandBuffer(VkCommandBuffer command,VkCommandBufferResetFlags flags){
    auto* d=device(command);const auto result=d->ResetCommandBuffer(command,flags);
    if(result==VK_SUCCESS)d->renderer.commandReset(command);else d->renderer.commandUnknown(command);return result;
}
VKAPI_ATTR void VKAPI_CALL cmdBindPipeline(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline){auto* d=device(command);d->renderer.pipelineBound(command,point,pipeline);d->CmdBindPipeline(command,point,pipeline);}
VKAPI_ATTR void VKAPI_CALL cmdBindDescriptorSets(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* offsets){auto* d=device(command);d->renderer.setsBound(command,point,first,count,sets,dynamicCount);d->CmdBindDescriptorSets(command,point,layout,first,count,sets,dynamicCount,offsets);}
bool bindDraw(Device& d,VkCommandBuffer command,const sbg::Draw& draw){
    if(!d.tracking.load(std::memory_order_acquire)||!draw.ready)return false;
    const bool coverage=draw.coverage||draw.control;
    if(!draw.original||!draw.replacement||(coverage&&(!draw.layout||!draw.coverage||!draw.control))){lost(d);return false;}
    d.CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.replacement);
    if(coverage){
        d.CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.layout,1,1,&draw.coverage,0,nullptr);
        d.CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.layout,2,1,&draw.control,0,nullptr);
    }return true;
}
VKAPI_ATTR void VKAPI_CALL cmdDraw(VkCommandBuffer command,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance){auto* d=device(command);const auto draw=d->renderer.beforeDraw(command);const bool changed=bindDraw(*d,command,draw);d->CmdDraw(command,vertices,instances,firstVertex,firstInstance);if(changed)d->CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.original);}
VKAPI_ATTR void VKAPI_CALL cmdDrawIndexed(VkCommandBuffer command,uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance){auto* d=device(command);const auto draw=d->renderer.beforeDraw(command);const bool changed=bindDraw(*d,command,draw);d->CmdDrawIndexed(command,indices,instances,firstIndex,vertexOffset,firstInstance);if(changed)d->CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,draw.original);}
VKAPI_ATTR void VKAPI_CALL cmdExecuteCommands(VkCommandBuffer command,uint32_t count,const VkCommandBuffer* commands){auto* d=device(command);d->renderer.commandUnknown(command);d->CmdExecuteCommands(command,count,commands);}
VKAPI_ATTR void VKAPI_CALL cmdBindShadersEXT(VkCommandBuffer command,uint32_t count,const VkShaderStageFlagBits* stages,const VkShaderEXT* shaders){auto* d=device(command);d->renderer.commandUnknown(command);d->CmdBindShadersEXT(command,count,stages,shaders);}
VKAPI_ATTR void VKAPI_CALL cmdPushDescriptorSetKHR(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t set,uint32_t count,const VkWriteDescriptorSet* writes){auto* d=device(command);d->renderer.commandUnknown(command);d->CmdPushDescriptorSetKHR(command,point,layout,set,count,writes);}
// Renderer owns the bounded submit-info copy and optional guard-CB prefix;
// it forwards the native fence and original queue submission exactly once.
VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo* submits,VkFence fence){return device(queue)->renderer.submit(queue,count,submits,fence);}
VKAPI_ATTR VkResult VKAPI_CALL queueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence){return device(queue)->renderer.submit2(queue,count,submits,fence,false);}
VKAPI_ATTR VkResult VKAPI_CALL queueSubmit2KHR(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence){return device(queue)->renderer.submit2(queue,count,submits,fence,true);}
PFN_vkVoidFunction hook(const char* name,PFN_vkVoidFunction original){
    if(!original||!sbg::enabled())return original;
#define HOOK(N,n) if(!std::strcmp(name,"vk" #N))return reinterpret_cast<PFN_vkVoidFunction>(n);
    HOOK(CreateShaderModule,createShaderModule) HOOK(DestroyShaderModule,destroyShaderModule)
    HOOK(CreateDescriptorSetLayout,createDescriptorSetLayout) HOOK(DestroyDescriptorSetLayout,destroyDescriptorSetLayout)
    HOOK(CreatePipelineLayout,createPipelineLayout) HOOK(DestroyPipelineLayout,destroyPipelineLayout)
    HOOK(CreateRenderPass,createRenderPass) HOOK(CreateRenderPass2,createRenderPass2) HOOK(CreateRenderPass2KHR,createRenderPass2KHR) HOOK(DestroyRenderPass,destroyRenderPass)
    HOOK(CreateGraphicsPipelines,createGraphicsPipelines) HOOK(DestroyPipeline,destroyPipeline)
    HOOK(CreateImage,createImage) HOOK(DestroyImage,destroyImage) HOOK(AllocateMemory,allocateMemory) HOOK(FreeMemory,freeMemory)
    HOOK(BindImageMemory,bindImageMemory) HOOK(BindImageMemory2,bindImageMemory2) HOOK(BindImageMemory2KHR,bindImageMemory2KHR) HOOK(GetMemoryFdKHR,getMemoryFdKHR)
    HOOK(CreateImageView,createImageView) HOOK(DestroyImageView,destroyImageView)
    HOOK(UpdateDescriptorSets,updateDescriptorSets) HOOK(UpdateDescriptorSetWithTemplate,updateDescriptorSetWithTemplate) HOOK(UpdateDescriptorSetWithTemplateKHR,updateDescriptorSetWithTemplateKHR)
    HOOK(AllocateDescriptorSets,allocateDescriptorSets) HOOK(FreeDescriptorSets,freeDescriptorSets) HOOK(ResetDescriptorPool,resetDescriptorPool) HOOK(DestroyDescriptorPool,destroyDescriptorPool)
    HOOK(GetDeviceQueue,getDeviceQueue) HOOK(GetDeviceQueue2,getDeviceQueue2)
    HOOK(CreateCommandPool,createCommandPool) HOOK(AllocateCommandBuffers,allocateCommandBuffers) HOOK(FreeCommandBuffers,freeCommandBuffers) HOOK(DestroyCommandPool,destroyCommandPool) HOOK(ResetCommandPool,resetCommandPool)
    HOOK(BeginCommandBuffer,beginCommandBuffer) HOOK(ResetCommandBuffer,resetCommandBuffer)
    HOOK(CmdBindPipeline,cmdBindPipeline) HOOK(CmdBindDescriptorSets,cmdBindDescriptorSets) HOOK(CmdDraw,cmdDraw) HOOK(CmdDrawIndexed,cmdDrawIndexed)
    HOOK(CmdExecuteCommands,cmdExecuteCommands) HOOK(CmdBindShadersEXT,cmdBindShadersEXT) HOOK(CmdPushDescriptorSetKHR,cmdPushDescriptorSetKHR)
    HOOK(QueueSubmit,queueSubmit) HOOK(QueueSubmit2,queueSubmit2) HOOK(QueueSubmit2KHR,queueSubmit2KHR)
#undef HOOK
    return original;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL physicalProc(VkInstance value,const char* name){auto* i=instance(value);return !i||!name?nullptr:(i->physical?i->physical(value,name):i->gipa(value,name));}
}
SFNP_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framebridge_vkGetDeviceProcAddr(VkDevice value,const char* name){
    auto* d=device(value);if(!d||!name)return nullptr;
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(framebridge_vkGetDeviceProcAddr);
    const auto original=d->gdpa(value,name);if(!original)return nullptr;
    if(!std::strcmp(name,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    return hook(name,original);
}
SFNP_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framebridge_vkGetInstanceProcAddr(VkInstance value,const char* name){
    if(!name)return nullptr;
    if(!std::strcmp(name,"vkGetInstanceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(framebridge_vkGetInstanceProcAddr);
    if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(framebridge_vkGetDeviceProcAddr);
    if(!std::strcmp(name,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
    auto* i=instance(value);if(!i)return nullptr;const auto original=i->gipa(value,name);if(!original)return nullptr;
    if(!std::strcmp(name,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    if(!std::strcmp(name,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
    if(!std::strcmp(name,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    return hook(name,original);
}
SFNP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL framebridge_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info){
    if(!info||info->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT||info->loaderLayerInterfaceVersion<2)return VK_ERROR_INITIALIZATION_FAILED;
    sbg::prepareProcessCold();info->loaderLayerInterfaceVersion=2;
    info->pfnGetInstanceProcAddr=framebridge_vkGetInstanceProcAddr;info->pfnGetDeviceProcAddr=framebridge_vkGetDeviceProcAddr;info->pfnGetPhysicalDeviceProcAddr=physicalProc;return VK_SUCCESS;
}
