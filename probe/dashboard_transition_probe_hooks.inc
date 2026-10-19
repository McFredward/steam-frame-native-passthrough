// Included only by the isolated diagnostic target, inside the loader namespace.
// These wrappers never change API arguments, return values or draw output.
VKAPI_ATTR void VKAPI_CALL dashboardGetDeviceQueue(VkDevice device,std::uint32_t family,std::uint32_t index,VkQueue* out){
    auto* d=deviceFor(device);d->GetDeviceQueue(device,family,index,out);d->dashboardProbe.queueRetrieved(family,index,*out);
}
VKAPI_ATTR void VKAPI_CALL dashboardGetDeviceQueue2(VkDevice device,const VkDeviceQueueInfo2* info,VkQueue* out){
    auto* d=deviceFor(device);d->GetDeviceQueue2(device,info,out);
    d->dashboardProbe.queueRetrieved(info->queueFamilyIndex,info->queueIndex,*out,!info->pNext&&!info->flags);
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardAllocateCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){
    auto* d=deviceFor(device);const auto result=d->AllocateCommandBuffers(device,info,out);d->dashboardProbe.commandsAllocated(*info,out,result);return result;
}
VKAPI_ATTR void VKAPI_CALL dashboardFreeCommandBuffers(VkDevice device,VkCommandPool pool,std::uint32_t count,const VkCommandBuffer* values){
    auto* d=deviceFor(device);d->dashboardProbe.commandsFreed(pool,count,values);d->FreeCommandBuffers(device,pool,count,values);
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardBeginCommandBuffer(VkCommandBuffer command,const VkCommandBufferBeginInfo* info){
    auto* d=deviceFor(command);const auto result=d->BeginCommandBuffer(command,info);d->dashboardProbe.begin(command,*info,result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardEndCommandBuffer(VkCommandBuffer command){
    auto* d=deviceFor(command);const auto result=d->EndCommandBuffer(command);d->dashboardProbe.end(command,result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardResetCommandBuffer(VkCommandBuffer command,VkCommandBufferResetFlags flags){
    auto* d=deviceFor(command);const auto result=d->ResetCommandBuffer(command,flags);d->dashboardProbe.reset(command,result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardResetCommandPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags){
    auto* d=deviceFor(device);const auto result=d->ResetCommandPool(device,pool,flags);if(result==VK_SUCCESS)d->dashboardProbe.poolRetired(pool);return result;
}
VKAPI_ATTR void VKAPI_CALL dashboardDestroyCommandPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* allocator){
    auto* d=deviceFor(device);d->dashboardProbe.poolRetired(pool,true);d->DestroyCommandPool(device,pool,allocator);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBindPipeline(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline){
    auto* d=deviceFor(command);d->dashboardProbe.bind(command,point,pipeline);d->CmdBindPipeline(command,point,pipeline);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBindDescriptorSets(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamicCount,const std::uint32_t* offsets){
    auto* d=deviceFor(command);d->dashboardProbe.nativeDescriptorBinding(command,point,first,count);d->CmdBindDescriptorSets(command,point,layout,first,count,sets,dynamicCount,offsets);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdPushDescriptorSetKHR(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t set,std::uint32_t count,const VkWriteDescriptorSet* writes){
    auto* d=deviceFor(command);d->dashboardProbe.nativeDescriptorBinding(command,point,set,1);d->CmdPushDescriptorSetKHR(command,point,layout,set,count,writes);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer command,VkDescriptorUpdateTemplate update,VkPipelineLayout layout,std::uint32_t set,const void* data){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdPushDescriptorSetWithTemplateKHR(command,update,layout,set,data);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBindDescriptorBuffersEXT(VkCommandBuffer command,std::uint32_t count,const VkDescriptorBufferBindingInfoEXT* info){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdBindDescriptorBuffersEXT(command,count,info);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,const std::uint32_t* indices,const VkDeviceSize* offsets){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdSetDescriptorBufferOffsetsEXT(command,point,layout,first,count,indices,offsets);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBindDescriptorBufferEmbeddedSamplersEXT(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t set){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdBindDescriptorBufferEmbeddedSamplersEXT(command,point,layout,set);
}
#define DASH_INFO_HOOK(name,type) VKAPI_ATTR void VKAPI_CALL dashboard##name(VkCommandBuffer command,const type* info){auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->name(command,info);}
DASH_INFO_HOOK(CmdBindDescriptorSets2KHR,VkBindDescriptorSetsInfoKHR)
DASH_INFO_HOOK(CmdPushDescriptorSet2KHR,VkPushDescriptorSetInfoKHR)
DASH_INFO_HOOK(CmdPushDescriptorSetWithTemplate2KHR,VkPushDescriptorSetWithTemplateInfoKHR)
DASH_INFO_HOOK(CmdSetDescriptorBufferOffsets2EXT,VkSetDescriptorBufferOffsetsInfoEXT)
DASH_INFO_HOOK(CmdBindDescriptorBufferEmbeddedSamplers2EXT,VkBindDescriptorBufferEmbeddedSamplersInfoEXT)
#undef DASH_INFO_HOOK
VKAPI_ATTR void VKAPI_CALL dashboardCmdDraw(VkCommandBuffer command,std::uint32_t vertices,std::uint32_t instances,std::uint32_t firstVertex,std::uint32_t firstInstance){
    auto* d=deviceFor(command);const bool swapped=vertices&&instances&&d->dashboardProbe.beforeDraw(command);
    d->CmdDraw(command,vertices,instances,firstVertex,firstInstance);if(swapped)d->dashboardProbe.afterDraw(command);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdDrawIndexed(VkCommandBuffer command,std::uint32_t indices,std::uint32_t instances,std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance){
    auto* d=deviceFor(command);const bool swapped=indices&&instances&&d->dashboardProbe.beforeDraw(command);
    d->CmdDrawIndexed(command,indices,instances,firstIndex,vertexOffset,firstInstance);if(swapped)d->dashboardProbe.afterDraw(command);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBeginQuery(VkCommandBuffer command,VkQueryPool pool,std::uint32_t query,VkQueryControlFlags flags){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdBeginQuery(command,pool,query,flags);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBeginQueryIndexedEXT(VkCommandBuffer command,VkQueryPool pool,std::uint32_t query,VkQueryControlFlags flags,std::uint32_t index){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdBeginQueryIndexedEXT(command,pool,query,flags,index);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdExecuteCommands(VkCommandBuffer command,std::uint32_t count,const VkCommandBuffer* children){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdExecuteCommands(command,count,children);
}
VKAPI_ATTR void VKAPI_CALL dashboardCmdBindShadersEXT(VkCommandBuffer command,std::uint32_t count,const VkShaderStageFlagBits* stages,const VkShaderEXT* shaders){
    auto* d=deviceFor(command);d->dashboardProbe.unknown(command);d->CmdBindShadersEXT(command,count,stages,shaders);
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardQueueSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkFence fence){
    auto* d=deviceFor(queue);bool ordinary=count<=128&&(!count||submits);
    for(unsigned n=0;ordinary&&n<count;++n){
        const auto& submit=submits[n];
        ordinary=submit.sType==VK_STRUCTURE_TYPE_SUBMIT_INFO&&submit.commandBufferCount<=dashboard_transition_probe::MaximumCommands&&
            (!submit.commandBufferCount||submit.pCommandBuffers);
        if(submit.pNext){
            const auto* timeline=static_cast<const VkTimelineSemaphoreSubmitInfo*>(submit.pNext);
            ordinary=ordinary&&timeline->sType==VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO&&!timeline->pNext&&
                timeline->waitSemaphoreValueCount==submit.waitSemaphoreCount&&timeline->signalSemaphoreValueCount==submit.signalSemaphoreCount&&
                (!timeline->waitSemaphoreValueCount||timeline->pWaitSemaphoreValues)&&(!timeline->signalSemaphoreValueCount||timeline->pSignalSemaphoreValues);
        }
    }
    d->dashboardProbe.beforeSubmit(queue,ordinary,count,submits);return d->QueueSubmit(queue,count,submits,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardQueueSubmit2(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* submits,VkFence fence){
    auto* d=deviceFor(queue);d->dashboardProbe.beforeSubmit(queue,false);return d->QueueSubmit2(queue,count,submits,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL dashboardQueueSubmit2KHR(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* submits,VkFence fence){
    auto* d=deviceFor(queue);d->dashboardProbe.beforeSubmit(queue,false);return d->QueueSubmit2KHR(queue,count,submits,fence);
}
PFN_vkVoidFunction dashboardProbeIntercept(const char* name) noexcept {
#define DASH_HOOK(method) if(!std::strcmp(name,"vk" #method))return reinterpret_cast<PFN_vkVoidFunction>(dashboard##method);
    DASH_HOOK(GetDeviceQueue) DASH_HOOK(GetDeviceQueue2)
    DASH_HOOK(AllocateCommandBuffers) DASH_HOOK(FreeCommandBuffers) DASH_HOOK(BeginCommandBuffer) DASH_HOOK(EndCommandBuffer)
    DASH_HOOK(ResetCommandBuffer) DASH_HOOK(ResetCommandPool) DASH_HOOK(DestroyCommandPool)
    DASH_HOOK(CmdBindPipeline) DASH_HOOK(CmdDraw) DASH_HOOK(CmdDrawIndexed)
    DASH_HOOK(CmdBindDescriptorSets) DASH_HOOK(CmdPushDescriptorSetKHR) DASH_HOOK(CmdPushDescriptorSetWithTemplateKHR)
    DASH_HOOK(CmdBindDescriptorBuffersEXT) DASH_HOOK(CmdSetDescriptorBufferOffsetsEXT) DASH_HOOK(CmdBindDescriptorBufferEmbeddedSamplersEXT)
    DASH_HOOK(CmdBindDescriptorSets2KHR) DASH_HOOK(CmdPushDescriptorSet2KHR) DASH_HOOK(CmdPushDescriptorSetWithTemplate2KHR)
    DASH_HOOK(CmdSetDescriptorBufferOffsets2EXT) DASH_HOOK(CmdBindDescriptorBufferEmbeddedSamplers2EXT)
    DASH_HOOK(CmdBeginQuery) DASH_HOOK(CmdBeginQueryIndexedEXT) DASH_HOOK(CmdExecuteCommands) DASH_HOOK(CmdBindShadersEXT)
    DASH_HOOK(QueueSubmit) DASH_HOOK(QueueSubmit2) DASH_HOOK(QueueSubmit2KHR)
#undef DASH_HOOK
    return nullptr;
}
