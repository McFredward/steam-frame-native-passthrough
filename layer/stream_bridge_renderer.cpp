#include "stream_bridge_renderer.h"
#include "stream_bridge_process.h"
#include "stream_bridge_coverage_gpu.h"
#include "native_presenter_pipeline.h"
#include "pixel_ssbo_profile_transform.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
namespace gpu=stream_bridge_coverage_gpu;
namespace sb=stream_bridge;
namespace native=stream_bridge_native;
namespace local=stream_bridge_local;
namespace journal=frame_pixel_ssbo_journal;
namespace stream_bridge_renderer {
namespace {
template<class T>std::uint64_t handle(T value){std::uint64_t n=0;static_assert(sizeof value<=sizeof n,"bounded native handle");std::memcpy(&n,&value,sizeof value);return n;}
template<class T>T from(std::uint64_t value){T result{};std::memcpy(&result,&value,sizeof result);return result;}
template<class T,std::size_t N>T* find(std::array<T,N>& list,std::uint64_t value){for(auto& entry:list)if(entry.id==value)return &entry;return nullptr;}
template<class T,std::size_t N>T* add(std::array<T,N>& list,std::uint64_t value){if(!value||find(list,value))return nullptr;for(auto& entry:list)if(!entry.id){entry.id=value;return &entry;}return nullptr;}
bool current(const LocalPicture& picture){return picture.leases&&picture.connection&&picture.connection->load(std::memory_order_acquire)&&picture.leases->current(picture.metadata.lease);}
bool same(const sb::FrameDescriptor&a,const sb::FrameDescriptor&b){return a.sourceFrame==b.sourceFrame&&a.encoderGeneration==b.encoderGeneration&&a.codedWidth==b.codedWidth&&a.codedHeight==b.codedHeight;}
void coldNote(const char* message) noexcept{
    static std::atomic<unsigned> budget{0};
    if(budget.fetch_add(1,std::memory_order_relaxed)<24)
        std::fprintf(stderr,"[Steam Link Native Passthrough] %s\n",message);
}
}
struct Renderer::Impl {
    struct Memory{std::uint64_t id=0,generation=0,device=0,inode=0;int duplicate=-1;};
    struct Image{std::uint64_t id=0,generation=0,memory=0,memoryGeneration=0;std::uint32_t width=0,height=0;};
    struct View{std::uint64_t id=0,image=0,generation=0,serial=0;};
    struct Descriptor{std::uint64_t id=0,view=0,viewSerial=0;bool valid=false;};
    struct Pipeline{std::uint64_t id=0;VkPipeline coverage{},legacy{};VkPipelineLayout layout{};};
    struct Use{gpu::Lease lease{};LocalPicture picture;std::uint64_t descriptor=0,viewSerial=0,image=0,imageGeneration=0,memoryGeneration=0;};
    struct Control{VkBuffer buffer{};VkDeviceSize offset=0;VkDescriptorSet set{};VkCommandBuffer one{};bool assigned=false;};
    struct Command{
        std::uint64_t id=0,pipeline=0,set0=0,epoch=0;bool primary=false,eligible=false;
        Control* control=nullptr;
        std::array<Use,gpu::MaximumSlots> uses{};std::size_t used=0;
    };
    std::atomic<std::uint64_t> observationEpoch{1};std::atomic<bool> lost{false};std::atomic<unsigned> admittedPipelines{0};
    mutable std::mutex gate;VkDevice device{};PFN_vkGetDeviceProcAddr gdpa{};VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t api=0,family=UINT32_MAX;std::uint64_t generation=0;bool prepared=false,registered=false,count1Authorized=false;
    gpu::Pool pool;journal::Journal definitions{1,{},journal::PendingCount1::RetainFacts};
    std::vector<std::uint32_t> coverageWords,legacyWords;
    VkDescriptorSetLayout controlLayout{};VkDescriptorPool controlPool{};VkCommandPool guardPool{};
    VkCommandBuffer zero{};VkQueue queue{};VkBuffer controlsBuffer{};VkDeviceMemory controlsMemory{};std::array<Control,128> controls{};
    std::array<Memory,64> memories{};std::array<Image,64> images{};std::array<View,256> views{};std::array<Descriptor,256> descriptors{};
    std::array<Pipeline,128> pipelines{};std::array<Command,128> commands{};std::array<std::shared_ptr<const LocalPicture>,16> pictures{};
#define FUNCS(X) X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(DeviceWaitIdle) \
 X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdUpdateBuffer) X(CmdFillBuffer) X(QueueSubmit)
#define FIELD(n) PFN_vk##n n=nullptr;
    FUNCS(FIELD)
#undef FIELD
    PFN_vkQueueSubmit2 QueueSubmit2=nullptr,QueueSubmit2KHR=nullptr;
    std::array<gpu::Lease,gpu::MaximumSlots> submittedLeases{};std::size_t submittedCount=0;
    bool load(){
#define LOAD(n) n=reinterpret_cast<PFN_vk##n>(gdpa(device,"vk" #n));if(!n)return false;
        FUNCS(LOAD)
#undef LOAD
        return true;
    }
    void release(Command& command){
        for(std::size_t n=0;n<command.used;++n)pool.releaseRecorded(command.uses[n].lease);
        command.uses={};command.used=0;command.pipeline=command.set0=0;command.eligible=false;
    }
    void barrier(VkCommandBuffer cb,Control& control,bool before){
        VkBufferMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        memoryBarrier.buffer=control.buffer;memoryBarrier.offset=control.offset;memoryBarrier.size=16;
        memoryBarrier.srcQueueFamilyIndex=memoryBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.srcAccessMask=before?VK_ACCESS_UNIFORM_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask=before?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_UNIFORM_READ_BIT;
        CmdPipelineBarrier(cb,before?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
            before?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&memoryBarrier,0,nullptr);
    }
    bool makeControls(){
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;
        if(CreateCommandPool(device,&cp,nullptr,&guardPool)!=VK_SUCCESS)return false;
        std::array<VkCommandBuffer,129> cbs{};VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool=guardPool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=cbs.size();
        if(AllocateCommandBuffers(device,&ca,cbs.data())!=VK_SUCCESS)return false;
        zero=cbs[0];VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if(BeginCommandBuffer(zero,&begin)!=VK_SUCCESS)return false;
        // Vulkan 1.1 guarantees minUniformBufferOffsetAlignment <= 256.
        // One fixed allocation, one zero fill and two barriers per native
        // submit replace 128 redundant clears/barrier pairs on mobile GPUs.
        constexpr VkDeviceSize stride=256;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=controls.size()*stride;
        buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(CreateBuffer(device,&buffer,nullptr,&controlsBuffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements requirements{};GetBufferMemoryRequirements(device,controlsBuffer,&requirements);unsigned type=UINT32_MAX;
        for(unsigned n=0;n<memory.memoryTypeCount;++n)if(requirements.memoryTypeBits&(1u<<n)){type=n;break;}
        if(type==UINT32_MAX)return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
        if(AllocateMemory(device,&allocation,nullptr,&controlsMemory)!=VK_SUCCESS||BindBufferMemory(device,controlsBuffer,controlsMemory,0)!=VK_SUCCESS)return false;
        VkBufferMemoryBarrier all{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};all.buffer=controlsBuffer;all.size=VK_WHOLE_SIZE;
        all.srcQueueFamilyIndex=all.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        all.srcAccessMask=VK_ACCESS_UNIFORM_READ_BIT;all.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        CmdPipelineBarrier(zero,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&all,0,nullptr);
        CmdFillBuffer(zero,controlsBuffer,0,VK_WHOLE_SIZE,0);
        all.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;all.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT;
        CmdPipelineBarrier(zero,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&all,0,nullptr);
        for(std::size_t k=0;k<controls.size();++k){auto& control=controls[k];control.one=cbs[k+1];control.buffer=controlsBuffer;control.offset=k*stride;
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=controlPool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&controlLayout;
            if(AllocateDescriptorSets(device,&allocate,&control.set)!=VK_SUCCESS)return false;
            VkDescriptorBufferInfo info{control.buffer,control.offset,16};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=control.set;write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&info;UpdateDescriptorSets(device,1,&write,0,nullptr);
            // These immutable commands own their value bytes. Native flags=0
            // recordings can repeat; no host write can race a pending GPU read.
            const float on=1;
            if(BeginCommandBuffer(control.one,&begin)!=VK_SUCCESS)return false;
            barrier(control.one,control,true);CmdUpdateBuffer(control.one,control.buffer,control.offset,sizeof(on),&on);barrier(control.one,control,false);
            if(EndCommandBuffer(control.one)!=VK_SUCCESS)return false;
        }
        return EndCommandBuffer(zero)==VK_SUCCESS;
    }
    void destroyControls(){
        if(guardPool)DestroyCommandPool(device,guardPool,nullptr);
        if(controlsBuffer)DestroyBuffer(device,controlsBuffer,nullptr);
        if(controlsMemory)FreeMemory(device,controlsMemory,nullptr);
        controlsBuffer={};controlsMemory={};for(auto& control:controls)control={};
        guardPool={};zero={};
    }
    static bool admitted(void* owner) noexcept{
        auto&p=*static_cast<Impl*>(owner);return !p.lost.load(std::memory_order_acquire)&&p.admittedPipelines.load(std::memory_order_acquire)>0;
    }
    static void coverage(void* owner,const sb::CertifiedAccessUnit& certificate) noexcept{
        auto&p=*static_cast<Impl*>(owner);try{std::vector<std::uint8_t> bytes;if(sb::decodeCertifiedCoverage(certificate,bytes))p.pool.upload({certificate.binding,certificate.descriptor},certificate.descriptor.codedWidth,certificate.descriptor.codedHeight,{bytes.data(),bytes.size()});}catch(...){}
    }
    static void picture(void* owner,const LocalPicture& value) noexcept{
        auto&p=*static_cast<Impl*>(owner);try{auto snapshot=std::make_shared<const LocalPicture>(value);std::atomic_store_explicit(&p.pictures[value.metadata.descriptor.sourceFrame%p.pictures.size()],std::move(snapshot),std::memory_order_release);}catch(...){}
    }
    static void tick(void* owner) noexcept{static_cast<Impl*>(owner)->pool.reapCold();}
    bool matchingImage(std::uint64_t image,std::shared_ptr<const LocalPicture>& output){
        const auto* i=find(images,image);if(!i)return false;const auto* m=find(memories,i->memory);if(!m||m->generation!=i->memoryGeneration||m->duplicate<0)return false;
        output.reset();
        for(const auto& slot:pictures){auto picture=std::atomic_load_explicit(&slot,std::memory_order_acquire);if(picture&&current(*picture)&&picture->metadata.lease.kernel==local::KernelIdentity{{m->device,m->inode}}&&picture->metadata.descriptor.codedWidth==i->width&&picture->metadata.descriptor.codedHeight==i->height){if(output)return false;output=std::move(picture);}}
        return output!=nullptr;
    }
    bool collect(VkCommandBuffer cb){
        auto* command=find(commands,handle(cb));if(!command||!command->used)return false;
        bool valid=command->eligible&&!lost.load(std::memory_order_acquire)&&command->epoch==observationEpoch.load(std::memory_order_acquire);
        for(std::size_t n=0;n<command->used;++n){auto& use=command->uses[n];valid=valid&&current(use.picture);
            // A descriptor can legally change after recording but before
            // submission. It must still name this EXACT leased image.
            const auto* descriptor=find(descriptors,use.descriptor);
            const auto* view=descriptor&&descriptor->valid?find(views,descriptor->view):nullptr;
            const auto* image=view?find(images,view->image):nullptr;
            const auto* memory=image?find(memories,image->memory):nullptr;
            valid=valid&&view&&view->serial==use.viewSerial&&descriptor->viewSerial==use.viewSerial&&
                image&&image->id==use.image&&image->generation==use.imageGeneration&&view->generation==use.imageGeneration&&
                memory&&memory->generation==use.memoryGeneration&&image->memoryGeneration==use.memoryGeneration&&
                local::KernelIdentity{{memory->device,memory->inode}}==use.picture.metadata.lease.kernel;
            bool seen=false;for(std::size_t k=0;k<submittedCount;++k)seen|=submittedLeases[k].slot==use.lease.slot&&submittedLeases[k].generation==use.lease.generation;
            if(!seen){if(submittedCount==submittedLeases.size())valid=false;else submittedLeases[submittedCount++]=use.lease;}
        }
        return valid;
    }

};
Renderer::Renderer():impl_(new Impl){}
Renderer::~Renderer()=default;
frame_pixel_ssbo_journal::Journal& Renderer::definitions() noexcept{return impl_->definitions;}
void Renderer::shaderCreated(VkShaderModule value,const VkShaderModuleCreateInfo& info) noexcept{
    auto&p=*impl_;try{
        if(!p.definitions.createShader(value,info,VK_SUCCESS)||!p.prepared||info.codeSize!=6620)return;
        std::lock_guard<std::mutex> lock(p.gate);if(p.count1Authorized)return;
        auto original=std::make_unique<frame_pixel_ssbo_clone::ShaderDefinition>();
        auto proof=std::make_unique<frame_pixel_ssbo_profile::Artifact>();
        frame_pixel_ssbo_clone::Refusal why;
        if(frame_pixel_ssbo_clone::captureShader(info,frame_pixel_ssbo_clone::ShaderRole::Fragment,*original,why)&&
           frame_pixel_ssbo_profile::transform(*original,false,*proof,why)&&
           p.definitions.authorizeProbeInterface(proof->profile)){
            p.count1Authorized=true;coldNote("native count1 image interface verified");
        }else coldNote("native count1 image interface refused; native pipeline retained");
    }catch(...){coldNote("native shader admission unavailable; native pipeline retained");}
}
bool Renderer::prepareCold(VkDevice device,VkPhysicalDeviceMemoryProperties memory,PFN_vkGetDeviceProcAddr get,const VkDeviceCreateInfo& create,std::uint32_t api,std::uint32_t family) noexcept{
    auto&p=*impl_;try{
        if(p.device)return false;
        p.gdpa=get;p.device=device;
        p.QueueSubmit=reinterpret_cast<PFN_vkQueueSubmit>(get(device,"vkQueueSubmit"));
        p.QueueSubmit2=reinterpret_cast<PFN_vkQueueSubmit2>(get(device,"vkQueueSubmit2"));
        p.QueueSubmit2KHR=reinterpret_cast<PFN_vkQueueSubmit2>(get(device,"vkQueueSubmit2KHR"));
        if(!enabled())return false;
        p.device=device;p.memory=memory;p.gdpa=get;p.api=api;p.family=family;
        p.definitions.createDevice(device,create,VK_SUCCESS);
        if(processKind()==ProcessKind::Vrlink)return true;
        if(family==UINT32_MAX||api<VK_API_VERSION_1_1||!p.load()||!readCoverageFragmentCold(p.coverageWords)||!readLegacyFragmentCold(p.legacyWords)||!p.pool.prepare(device,get,memory,family))return false;
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=1;layout.pBindings=&binding;
        if(p.CreateDescriptorSetLayout(device,&layout,nullptr,&p.controlLayout)!=VK_SUCCESS)return false;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,128};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;pool.maxSets=128;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        if(p.CreateDescriptorPool(device,&pool,nullptr,&p.controlPool)!=VK_SUCCESS)return false;
        // Exactly one requested nonprotected queue in this admitted graphics
        // family. Other families cannot record one of our eligible primaries.
        unsigned queues=0;for(unsigned n=0;n<create.queueCreateInfoCount;++n)if(create.pQueueCreateInfos[n].queueFamilyIndex==family&&!create.pQueueCreateInfos[n].flags)queues+=create.pQueueCreateInfos[n].queueCount;
        if(queues!=1||!p.makeControls())return false;
        p.GetDeviceQueue(device,family,0,&p.queue);if(!p.queue)return false;
        p.prepared=true;return true;
    }catch(...){return false;}
}
void Renderer::graphicsCreated(VkPipelineCache cache,const VkGraphicsPipelineCreateInfo& info,const VkAllocationCallbacks* allocator,VkPipeline original) noexcept{
    auto&p=*impl_;if(!p.prepared)return;
    (void)allocator;
    try{journal::Event event;if(!p.definitions.createPipeline(original,info,VK_SUCCESS,&event)){
            if(event.contract.reason==frame_pixel_ssbo_clone::Reason::Image9Extent)coldNote("native pipeline refused: image interface not verified");
            return;
        }auto ready=std::make_unique<journal::Ready>();if(!p.definitions.lookupPipeline({handle(p.device),event.value,event.generation},*ready))return;
        if(frame_native_presenter::inspectCompletedPipeline(p.device,p.api,VK_SUCCESS,info,original,ready->recipe).reason!=frame_native_presenter::Reason::None)return;
        std::lock_guard<std::mutex> lock(p.gate);auto* entry=add(p.pipelines,handle(original));if(!entry)return;
        VkDescriptorSetLayout layouts[3]{from<VkDescriptorSetLayout>(ready->recipe.provenance[0].value),p.pool.descriptorLayout(),p.controlLayout};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};ci.setLayoutCount=3;ci.pSetLayouts=layouts;ci.pushConstantRangeCount=ready->recipe.layout.count;ci.pPushConstantRanges=ready->recipe.layout.ranges.data();
        if(p.CreatePipelineLayout(p.device,&ci,nullptr,&entry->layout)!=VK_SUCCESS){*entry={};return;}
        auto create=[&](const std::vector<std::uint32_t>& words,VkPipelineLayout layout,VkPipeline& pipeline){
            VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=words.size()*4;shader.pCode=words.data();VkShaderModule module{};if(p.CreateShaderModule(p.device,&shader,nullptr,&module)!=VK_SUCCESS)return;
            auto candidate=info;std::array<VkPipelineShaderStageCreateInfo,2> stages{{info.pStages[0],info.pStages[1]}};for(auto& stage:stages)if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT)stage.module=module;candidate.pStages=stages.data();candidate.layout=layout;
            const auto result=p.CreateGraphicsPipelines(p.device,cache,1,&candidate,nullptr,&pipeline);p.DestroyShaderModule(p.device,module,nullptr);if(result!=VK_SUCCESS&&pipeline){p.DestroyPipeline(p.device,pipeline,nullptr);pipeline={};}
        };
        create(p.coverageWords,entry->layout,entry->coverage);create(p.legacyWords,info.layout,entry->legacy);
        if(entry->coverage&&entry->legacy){
            p.admittedPipelines.fetch_add(1,std::memory_order_release);
            if(!p.registered)p.registered=registerConsumerCold({&p,Impl::coverage,Impl::picture,Impl::tick,Impl::admitted});
            coldNote("native legacy and exact-coverage pipelines prepared");
        }else coldNote("native companion creation refused; native pipeline retained");
    }catch(...){/* Native original has already succeeded and remains untouched. */}
}
void Renderer::pipelineDestroyed(VkPipeline original) noexcept{
    auto&p=*impl_;try{p.definitions.destroyPipeline(original);std::lock_guard<std::mutex> lock(p.gate);if(auto* entry=find(p.pipelines,handle(original))){if(entry->coverage&&entry->legacy)p.admittedPipelines.fetch_sub(1,std::memory_order_release);if(entry->coverage)p.DestroyPipeline(p.device,entry->coverage,nullptr);if(entry->legacy)p.DestroyPipeline(p.device,entry->legacy,nullptr);if(entry->layout)p.DestroyPipelineLayout(p.device,entry->layout,nullptr);*entry={};}}catch(...){}
}
void Renderer::imageCreated(VkImage value,const VkImageCreateInfo& info) noexcept{
    auto&p=*impl_;bool dma=false;auto* node=static_cast<const VkBaseInStructure*>(info.pNext);
    for(unsigned n=0;node&&n<32;++n,node=node->pNext)if(node->sType==VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)dma=(reinterpret_cast<const VkExternalMemoryImageCreateInfo*>(node)->handleTypes&VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)!=0;
    if(auto* source=sourceImages())source->created(handle(p.device),handle(value),{info.extent.width,info.extent.height,std::uint32_t(info.format),dma});
    if(!p.prepared||!dma||info.imageType!=VK_IMAGE_TYPE_2D||info.samples!=VK_SAMPLE_COUNT_1_BIT||info.mipLevels!=1||info.arrayLayers!=1)return;
    std::lock_guard<std::mutex> lock(p.gate);if(auto* image=add(p.images,handle(value))){image->generation=++p.generation;image->width=info.extent.width;image->height=info.extent.height;}
}
void Renderer::imageDestroyed(VkImage value) noexcept{auto&p=*impl_;if(auto* source=sourceImages())source->destroyed(handle(p.device),handle(value));std::lock_guard<std::mutex> lock(p.gate);if(auto* image=find(p.images,handle(value)))*image={};}
MemoryPreparation Renderer::memoryPreparing(const VkMemoryAllocateInfo& info) noexcept{
    MemoryPreparation result;if(!impl_->prepared)return result;
    auto* node=static_cast<const VkBaseInStructure*>(info.pNext);
    for(unsigned n=0;node&&n<32;++n,node=node->pNext)if(node->sType==VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR){
        const auto& imported=*reinterpret_cast<const VkImportMemoryFdInfoKHR*>(node);
        if(imported.handleType!=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT||imported.fd<0)return result;
        // The original allocation consumes its fd on SUCCESS. Retain this exact
        // kernel object BEFORE forwarding, never stat the potentially reused fd.
        result.duplicate=fcntl(imported.fd,F_DUPFD_CLOEXEC,3);struct stat file{};
        if(result.duplicate<0||fstat(result.duplicate,&file)){if(result.duplicate>=0)close(result.duplicate);result={};return result;}
        result.device=std::uint64_t(file.st_dev);result.inode=std::uint64_t(file.st_ino);return result;
    }return result;
}
void Renderer::memoryAllocated(VkDeviceMemory value,const VkMemoryAllocateInfo& info,MemoryPreparation& preparation,VkResult original) noexcept{
    auto&p=*impl_;if(auto* source=sourceImages()){bool exported=false;auto* node=static_cast<const VkBaseInStructure*>(info.pNext);for(unsigned n=0;node&&n<32;++n,node=node->pNext)if(node->sType==VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)exported=(reinterpret_cast<const VkExportMemoryAllocateInfo*>(node)->handleTypes&VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)!=0;if(original==VK_SUCCESS)source->allocated(handle(p.device),handle(value),exported);}
    if(preparation.duplicate<0)return;
    std::lock_guard<std::mutex> lock(p.gate);auto* memory=original==VK_SUCCESS?add(p.memories,handle(value)):nullptr;
    if(memory){memory->generation=++p.generation;memory->device=preparation.device;memory->inode=preparation.inode;memory->duplicate=preparation.duplicate;preparation.duplicate=-1;}
    if(preparation.duplicate>=0)close(preparation.duplicate);
    preparation={};
}
void Renderer::memoryFreed(VkDeviceMemory value) noexcept{auto&p=*impl_;if(auto* source=sourceImages())source->freed(handle(p.device),handle(value));std::lock_guard<std::mutex> lock(p.gate);if(auto* memory=find(p.memories,handle(value))){if(memory->duplicate>=0)close(memory->duplicate);*memory={};}}
void Renderer::imageBound(VkImage value,VkDeviceMemory memory,VkDeviceSize offset) noexcept{auto&p=*impl_;if(auto* source=sourceImages())source->bound(handle(p.device),handle(value),handle(memory),offset);std::lock_guard<std::mutex> lock(p.gate);auto* image=find(p.images,handle(value));const auto* allocation=find(p.memories,handle(memory));if(image&&allocation&&!offset){image->memory=allocation->id;image->memoryGeneration=allocation->generation;}}
void Renderer::memoryExported(VkDeviceMemory memory,int fd,VkExternalMemoryHandleTypeFlagBits type,std::uintptr_t caller) noexcept{auto&p=*impl_;if(auto* source=sourceImages())if(source->exported(handle(p.device),handle(memory),fd,std::uint32_t(type),caller))sourceExportedFd(fd);}
void Renderer::viewCreated(VkImageView value,const VkImageViewCreateInfo& info) noexcept{auto&p=*impl_;if(!p.prepared)return;std::lock_guard<std::mutex> lock(p.gate);const auto* image=find(p.images,handle(info.image));if(!image||info.viewType!=VK_IMAGE_VIEW_TYPE_2D||info.subresourceRange.baseMipLevel||info.subresourceRange.levelCount!=1||info.subresourceRange.baseArrayLayer||info.subresourceRange.layerCount!=1)return;if(auto* view=add(p.views,handle(value))){view->image=image->id;view->generation=image->generation;view->serial=++p.generation;}}
void Renderer::viewDestroyed(VkImageView value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);if(auto* view=find(p.views,handle(value)))*view={};}
void Renderer::descriptorWrites(std::uint32_t count,const VkWriteDescriptorSet* writes,std::uint32_t copies,const VkCopyDescriptorSet*) noexcept{
    auto&p=*impl_;if(!p.prepared)return;std::lock_guard<std::mutex> lock(p.gate);if(count>256||copies){for(auto& entry:p.descriptors)entry.valid=false;return;}
    for(unsigned n=0;n<count;++n){const auto& write=writes[n];if(write.dstBinding!=9)continue;auto* entry=find(p.descriptors,handle(write.dstSet));if(!entry)entry=add(p.descriptors,handle(write.dstSet));if(!entry)continue;entry->valid=false;
        if(write.sType==VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET&&!write.pNext&&write.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE&&!write.dstArrayElement&&write.descriptorCount&&write.descriptorCount<=3&&write.pImageInfo){entry->view=handle(write.pImageInfo[0].imageView);const auto* view=find(p.views,entry->view);entry->valid=view!=nullptr;entry->viewSerial=view?view->serial:0;}
    }
}
void Renderer::descriptorRetired(VkDescriptorSet value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);if(auto* entry=find(p.descriptors,handle(value)))*entry={};}
void Renderer::descriptorsInvalidated() noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);for(auto& entry:p.descriptors)entry={};}
void Renderer::queueObserved(VkQueue,std::uint32_t) noexcept{}
void Renderer::commandAllocated(VkCommandBuffer value,VkCommandBufferLevel level,std::uint32_t family) noexcept{
    auto&p=*impl_;if(!p.prepared)return;try{std::lock_guard<std::mutex> lock(p.gate);if(auto* command=add(p.commands,handle(value))){command->primary=level==VK_COMMAND_BUFFER_LEVEL_PRIMARY&&family==p.family;if(command->primary){for(auto& control:p.controls)if(!control.assigned){control.assigned=true;command->control=&control;break;}if(!command->control)command->primary=false;}}}catch(...){}
}
void Renderer::commandRetired(VkCommandBuffer value) noexcept{auto&p=*impl_;std::lock_guard<std::mutex> lock(p.gate);if(auto* command=find(p.commands,handle(value))){p.release(*command);if(command->control)command->control->assigned=false;*command={};}}
void Renderer::commandBegin(VkCommandBuffer value,const VkCommandBufferBeginInfo* info) noexcept{
    auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}
    if(auto* command=find(p.commands,handle(value))){p.release(*command);command->eligible=command->primary&&info&&info->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO&&!info->pNext&&
        !(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);command->epoch=p.observationEpoch.load(std::memory_order_acquire);}
}
void Renderer::pipelineBound(VkCommandBuffer value,VkPipelineBindPoint point,VkPipeline pipeline) noexcept{auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}if(auto* command=find(p.commands,handle(value))){command->pipeline=point==VK_PIPELINE_BIND_POINT_GRAPHICS?handle(pipeline):0;}}
void Renderer::setsBound(VkCommandBuffer value,VkPipelineBindPoint point,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamic) noexcept{auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}if(auto* command=find(p.commands,handle(value))){if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS||dynamic){command->set0=0;return;}if(!first&&count&&sets)command->set0=handle(sets[0]);}}
void Renderer::commandUnknown(VkCommandBuffer value) noexcept{auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}if(auto* command=find(p.commands,handle(value))){command->eligible=false;command->pipeline=command->set0=0;}}
Draw Renderer::beforeDraw(VkCommandBuffer value) noexcept{
    auto&p=*impl_;Draw out;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);if(!lock||!p.prepared||p.lost.load(std::memory_order_acquire))return out;
    auto* command=find(p.commands,handle(value));if(!command||command->epoch!=p.observationEpoch.load(std::memory_order_acquire))return out;auto* pipeline=find(p.pipelines,command->pipeline);if(!pipeline)return out;
    out.original=from<VkPipeline>(pipeline->id);
    // Old atlas recognition remains the safe original-route companion while a
    // new exact frame/upload is unavailable. Non-atlas native RGB passes through.
    if(pipeline->legacy){out.replacement=pipeline->legacy;out.ready=true;}
    if(!command->eligible||!command->control||!pipeline->coverage)return out;
    const auto* descriptor=find(p.descriptors,command->set0);if(!descriptor||!descriptor->valid)return out;
    const auto* view=find(p.views,descriptor->view);if(!view||view->serial!=descriptor->viewSerial)return out;const auto* image=find(p.images,view->image);if(!image||image->generation!=view->generation)return out;
    std::shared_ptr<const LocalPicture> picture;if(!p.matchingImage(image->id,picture))return out;
    Impl::Use* use=nullptr;
    for(unsigned n=0;n<command->used;++n)if(command->uses[n].descriptor==command->set0&&command->uses[n].viewSerial==view->serial&&command->uses[n].picture.metadata.binding==picture->metadata.binding&&same(command->uses[n].picture.metadata.descriptor,picture->metadata.descriptor)){use=&command->uses[n];break;}
    if(!use){if(command->used==command->uses.size())return out;auto lease=p.pool.acquireScheduled({picture->metadata.binding,picture->metadata.descriptor});if(!lease)return out;use=&command->uses[command->used++];use->lease=lease;use->picture=*picture;use->descriptor=command->set0;use->viewSerial=view->serial;use->image=image->id;use->imageGeneration=image->generation;use->memoryGeneration=image->memoryGeneration;}
    out.replacement=pipeline->coverage;out.layout=pipeline->layout;out.coverage=use->lease.set;out.control=command->control->set;out.ready=true;return out;
}
void Renderer::commandReset(VkCommandBuffer value) noexcept{
    auto&p=*impl_;std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    if(!lock){p.observationEpoch.fetch_add(1,std::memory_order_acq_rel);return;}
    if(auto* command=find(p.commands,handle(value)))p.release(*command);
}
void Renderer::trackingLost() noexcept{impl_->lost.store(true,std::memory_order_release);}
VkResult Renderer::submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* infos,VkFence fence) noexcept{
    auto&p=*impl_;if(!p.QueueSubmit)return VK_ERROR_INITIALIZATION_FAILED;
    if(!p.prepared||queue!=p.queue)return p.QueueSubmit(queue,count,infos,fence);
    p.pool.drainUploads(queue);
    // Same native queue owner, bounded stack-only scratch, original semaphores,
    // pNext and fence retained. A leading immutable zero command makes even a
    // reused flags=0 CB fail closed. Only current exact leases get a one guard.
    std::array<VkSubmitInfo,65> submits{};std::array<VkCommandBuffer,1024> commands{};
    submits[0].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submits[0].commandBufferCount=1;submits[0].pCommandBuffers=&p.zero;
    std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);
    bool supported=lock.owns_lock()&&count<=64&&(infos||!count);std::size_t used=0;p.submittedCount=0;
    if(supported)for(unsigned n=0;n<count;++n){
        if(infos[n].pNext||infos[n].commandBufferCount>512||(!infos[n].pCommandBuffers&&infos[n].commandBufferCount)||used+infos[n].commandBufferCount*2>commands.size()){supported=false;break;}
        submits[n+1]=infos[n];submits[n+1].pCommandBuffers=commands.data()+used;submits[n+1].commandBufferCount=0;
        for(unsigned k=0;k<infos[n].commandBufferCount;++k){const auto cb=infos[n].pCommandBuffers[k];
            if(p.collect(cb)){auto* command=find(p.commands,handle(cb));if(command&&command->control){commands[used++]=command->control->one;++submits[n+1].commandBufferCount;}}
            commands[used++]=cb;++submits[n+1].commandBufferCount;
        }
    }
    if(!supported){
        // Unknown extension/cardinality never reuses an old successful guard.
        // The owned zero CB is immutable SIMULTANEOUS_USE and kept until idle.
        p.pool.prepareSubmit(queue,nullptr,gpu::MaximumSlots+1);
        if(count<=64&&(infos||!count)){
            for(unsigned n=0;n<count;++n)submits[n+1]=infos[n];
            return p.QueueSubmit(queue,count+1,submits.data(),fence);
        }
        if(!infos)return p.QueueSubmit(queue,count,infos,fence);
        // Large batches keep every original submit and its extension objects.
        // The caller still owns the same queue's external synchronization for
        // this whole call. Stack-only chunks preserve submission order and put
        // the original completion fence on the final chunk only.
        for(std::uint32_t at=0;at<count;){const auto chunk=std::min<std::uint32_t>(64,count-at);
            for(unsigned n=0;n<chunk;++n)submits[n+1]=infos[at+n];
            const auto result=p.QueueSubmit(queue,chunk+1,submits.data(),at+chunk==count?fence:VK_NULL_HANDLE);
            if(result!=VK_SUCCESS)return result;
            at+=chunk;
        }
        return VK_SUCCESS;
    }
    if(!p.pool.prepareSubmit(queue,p.submittedLeases.data(),p.submittedCount)){
        // Fenced retirement is part of admission, not just resource accounting.
        // Clear all one guards on refusal while retaining ordinary native RGB.
        for(std::size_t n=0;n<used;++n)for(const auto& control:p.controls)if(commands[n]==control.one)commands[n]=p.zero;
    }
    const auto result=p.QueueSubmit(queue,count+1,submits.data(),fence);
    p.pool.submitted(queue,p.submittedLeases.data(),p.submittedCount,result);p.submittedCount=0;return result;
}
VkResult Renderer::submit2(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* infos,VkFence fence,bool khr) noexcept{
    auto&p=*impl_;const auto forward=khr?p.QueueSubmit2KHR:p.QueueSubmit2;if(!forward)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!p.prepared||queue!=p.queue)return forward(queue,count,infos,fence);
    p.pool.drainUploads(queue);
    std::array<VkSubmitInfo2,65> submits{};std::array<VkCommandBufferSubmitInfo,1024> commands{};
    const VkCommandBufferSubmitInfo zero{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,nullptr,p.zero,1};
    submits[0].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2;submits[0].commandBufferInfoCount=1;submits[0].pCommandBufferInfos=&zero;
    std::unique_lock<std::mutex> lock(p.gate,std::try_to_lock);bool supported=lock.owns_lock()&&count<=64&&(infos||!count);std::size_t used=0;p.submittedCount=0;
    if(supported)for(unsigned n=0;n<count;++n){
        if(infos[n].pNext||infos[n].flags||infos[n].commandBufferInfoCount>512||(!infos[n].pCommandBufferInfos&&infos[n].commandBufferInfoCount)||used+infos[n].commandBufferInfoCount*2>commands.size()){supported=false;break;}
        submits[n+1]=infos[n];submits[n+1].pCommandBufferInfos=commands.data()+used;submits[n+1].commandBufferInfoCount=0;
        for(unsigned k=0;k<infos[n].commandBufferInfoCount;++k){const auto& cb=infos[n].pCommandBufferInfos[k];
            if(p.collect(cb.commandBuffer)){auto* command=find(p.commands,handle(cb.commandBuffer));if(command&&command->control){commands[used++]={VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,nullptr,command->control->one,1};++submits[n+1].commandBufferInfoCount;}}
            commands[used++]=cb;++submits[n+1].commandBufferInfoCount;
        }
    }
    if(!supported){
        p.pool.prepareSubmit(queue,nullptr,gpu::MaximumSlots+1);
        if(count<=64&&(infos||!count)){for(unsigned n=0;n<count;++n)submits[n+1]=infos[n];return forward(queue,count+1,submits.data(),fence);}
        if(!infos)return forward(queue,count,infos,fence);
        for(std::uint32_t at=0;at<count;){const auto chunk=std::min<std::uint32_t>(64,count-at);
            for(unsigned n=0;n<chunk;++n)submits[n+1]=infos[at+n];
            const auto result=forward(queue,chunk+1,submits.data(),at+chunk==count?fence:VK_NULL_HANDLE);
            if(result!=VK_SUCCESS)return result;
            at+=chunk;
        }
        return VK_SUCCESS;
    }
    if(!p.pool.prepareSubmit(queue,p.submittedLeases.data(),p.submittedCount)){
        for(std::size_t n=0;n<used;++n)for(const auto& control:p.controls)if(commands[n].commandBuffer==control.one)commands[n].commandBuffer=p.zero;
    }
    const auto result=forward(queue,count+1,submits.data(),fence);
    p.pool.submitted(queue,p.submittedLeases.data(),p.submittedCount,result);p.submittedCount=0;return result;
}
void Renderer::stopCold() noexcept{
    auto&p=*impl_;if(p.registered){unregisterConsumerCold(&p);p.registered=false;}
    std::lock_guard<std::mutex> lock(p.gate);if(!p.device)return;
    // Device destruction is a cold lifecycle boundary. Join the producer by
    // unregistering first, then obtain actual GPU idle before owned cleanup.
    if(p.DeviceWaitIdle&&p.DeviceWaitIdle(p.device)==VK_SUCCESS){
        for(auto& command:p.commands)p.release(command);
        p.pool.destroyAfterIdle();p.destroyControls();
        for(auto& entry:p.pipelines){if(entry.coverage)p.DestroyPipeline(p.device,entry.coverage,nullptr);if(entry.legacy)p.DestroyPipeline(p.device,entry.legacy,nullptr);if(entry.layout)p.DestroyPipelineLayout(p.device,entry.layout,nullptr);entry={};}
        if(p.controlPool)p.DestroyDescriptorPool(p.device,p.controlPool,nullptr);
        if(p.controlLayout)p.DestroyDescriptorSetLayout(p.device,p.controlLayout,nullptr);
    }else p.pool.abandonAfterDeviceDestroy(); // Native DestroyDevice owns eventual reclamation.
    for(auto& memory:p.memories)if(memory.duplicate>=0)close(memory.duplicate);
    p.prepared=false;p.device={};
}
} // namespace stream_bridge_renderer
