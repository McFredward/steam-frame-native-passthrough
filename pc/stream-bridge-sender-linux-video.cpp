#include "stream-bridge-sender-linux-video.h"
#include <cstring>
#include <limits>

namespace stream_bridge::sender {
namespace {
template<class T> const T* find(const void* p,VkStructureType type) {
    const T* result=nullptr;
    for(unsigned n=0;p&&n<16;++n){
        auto* b=static_cast<const VkBaseInStructure*>(p);
        if(b->sType==type){if(result)return nullptr;result=reinterpret_cast<const T*>(b);}
        p=b->pNext;
    }
    return p?nullptr:result;
}
bool extent(VkExtent2D e){return e.width&&e.height&&e.width<=16384&&e.height<=16384;}
}
std::uint64_t LinuxVideoBoundary::next(){return generation_==std::numeric_limits<std::uint64_t>::max()?0:++generation_;}
LinuxVideoBoundary::Session* LinuxVideoBoundary::session(VkVideoSessionKHR h){for(auto& x:sessions_)if(x.handle==h&&h)return &x;return nullptr;}
LinuxVideoBoundary::View* LinuxVideoBoundary::view(VkImageView h){for(auto& x:views_)if(x.handle==h&&h)return &x;return nullptr;}
LinuxVideoBoundary::Pool* LinuxVideoBoundary::pool(VkQueryPool h){for(auto& x:pools_)if(x.handle==h&&h)return &x;return nullptr;}
LinuxVideoBoundary::Command* LinuxVideoBoundary::command(VkCommandBuffer h){for(auto& x:commands_)if(x.handle==h&&h)return &x;return nullptr;}
LinuxVideoBoundary::Query* LinuxVideoBoundary::query(VkQueryPool h,std::uint32_t i){for(auto& x:queries_)if(x.pool==h&&h&&x.index==i)return &x;return nullptr;}
bool LinuxVideoBoundary::current(const LinuxEncodedRegion& r)const{
    bool s=false,v=false;
    for(const auto& x:sessions_)s|=x.handle==r.session&&x.generation==r.sessionGeneration&&x.handle;
    for(const auto& x:views_)v|=x.handle==r.view&&x.generation==r.viewGeneration&&x.handle;
    return s&&v&&r.device==device_;
}
VkResult LinuxVideoBoundary::createSession(const VkVideoSessionCreateInfoKHR* i,const VkAllocationCallbacks* a,VkVideoSessionKHR* out){
    auto status=d_.createSession(device_,i,a,out);if(status!=VK_SUCCESS||!i||!out||!*out)return status;
    const auto* profile=i->pVideoProfile;
    const auto* h=profile?find<VkVideoEncodeH265ProfileInfoKHR>(profile->pNext,VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR):nullptr;
    if(!profile||profile->videoCodecOperation!=VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR||!h||!extent(i->maxCodedExtent)||i->pictureFormat==VK_FORMAT_UNDEFINED||i->flags)return status;
    std::lock_guard<std::mutex> lock(gate_);
    if(session(*out))return status;
    for(auto& s:sessions_)if(!s.handle){auto n=next();if(n)s={*out,n,i->maxCodedExtent,i->pictureFormat};break;}
    return status;
}
void LinuxVideoBoundary::destroySession(VkVideoSessionKHR h,const VkAllocationCallbacks* a){
    {std::lock_guard<std::mutex> lock(gate_);if(auto* s=session(h))*s={};}
    d_.destroySession(device_,h,a);
}
VkResult LinuxVideoBoundary::createPool(const VkQueryPoolCreateInfo* i,const VkAllocationCallbacks* a,VkQueryPool* out){
    auto status=d_.createPool(device_,i,a,out);if(status!=VK_SUCCESS||!i||!out||!*out)return status;
    auto* f=find<VkQueryPoolVideoEncodeFeedbackCreateInfoKHR>(i->pNext,VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR);
    auto* p=find<VkVideoProfileInfoKHR>(i->pNext,VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR);
    constexpr auto flags=VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR|VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
    if(i->queryType!=VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR||!f||f->encodeFeedbackFlags!=flags||!p||p->videoCodecOperation!=VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR||!i->queryCount||i->queryCount>Limit)return status;
    std::lock_guard<std::mutex> lock(gate_);if(pool(*out))return status;
    for(auto& p0:pools_)if(!p0.handle){auto n=next();if(n)p0={*out,n,i->queryCount};break;}
    return status;
}
void LinuxVideoBoundary::destroyPool(VkQueryPool h,const VkAllocationCallbacks* a){
    {std::lock_guard<std::mutex> lock(gate_);if(auto* p=pool(h))*p={};for(auto& q:queries_)if(q.pool==h)q={};}
    d_.destroyPool(device_,h,a);
}
VkResult LinuxVideoBoundary::createView(const VkImageViewCreateInfo* i,const VkAllocationCallbacks* a,VkImageView* out){
    auto status=d_.createView(device_,i,a,out);if(status!=VK_SUCCESS||!i||!out||!*out||!i->image||i->format==VK_FORMAT_UNDEFINED||i->subresourceRange.levelCount!=1||!i->subresourceRange.layerCount||i->subresourceRange.layerCount==VK_REMAINING_ARRAY_LAYERS)return status;
    std::lock_guard<std::mutex> lock(gate_);if(view(*out))return status;
    for(auto& v:views_)if(!v.handle){auto n=next();if(n)v={*out,n,i->image,i->subresourceRange,i->format};break;}
    return status;
}
void LinuxVideoBoundary::destroyView(VkImageView h,const VkAllocationCallbacks* a){
    {std::lock_guard<std::mutex> lock(gate_);if(auto* v=view(h))*v={};}
    d_.destroyView(device_,h,a);
}
void LinuxVideoBoundary::destroyImage(VkImage image,const VkAllocationCallbacks* a){
    {std::lock_guard<std::mutex> lock(gate_);for(auto& v:views_)if(v.image==image)v={};}
    d_.destroyImage(device_,image,a);
}
void LinuxVideoBoundary::destroyBuffer(VkBuffer buffer,const VkAllocationCallbacks* a){
    {std::lock_guard<std::mutex> lock(gate_);for(auto& q:queries_)if(q.region.bitstream==buffer)q={};}
    d_.destroyBuffer(device_,buffer,a);
}
VkResult LinuxVideoBoundary::begin(VkCommandBuffer h,const VkCommandBufferBeginInfo* i){
    auto status=d_.begin(h,i);if(status!=VK_SUCCESS)return status;
    std::lock_guard<std::mutex> lock(gate_);auto* c=command(h);
    if(!c)for(auto& x:commands_)if(!x.handle){c=&x;break;}
    if(c){*c={};c->handle=h;c->generation=next();c->recording=c->generation!=0;c->valid=c->recording;}
    return status;
}
VkResult LinuxVideoBoundary::end(VkCommandBuffer h){auto status=d_.end(h);std::lock_guard<std::mutex> lock(gate_);if(auto* c=command(h)){c->recording=false;c->ended=status==VK_SUCCESS&&c->valid&&!c->pool;}return status;}
void LinuxVideoBoundary::beginVideo(VkCommandBuffer h,const VkVideoBeginCodingInfoKHR* i){
    d_.beginVideo(h,i);std::lock_guard<std::mutex> lock(gate_);if(auto* c=command(h)){if(!c->recording||!i||!session(i->videoSession))c->valid=false;else c->session=i->videoSession;}
}
void LinuxVideoBoundary::resetQueries(VkCommandBuffer h,VkQueryPool p,std::uint32_t first,std::uint32_t count){
    d_.resetQueries(h,p,first,count);std::lock_guard<std::mutex> lock(gate_);auto* c=command(h);auto* p0=pool(p);
    // Even an untracked recording must revoke an older observation of slots it resets.
    for(auto& q:queries_)if(q.pool==p&&q.index>=first&&q.index-first<count)q={};
    if(!c||!c->recording||!p0||!count||first>=p0->count||count>p0->count-first)return;
    for(std::uint32_t i=first;i<first+count;++i){
        auto* q=query(p,i);if(!q)for(auto& x:queries_)if(!x.pool){q=&x;break;}
        if(!q){c->valid=false;return;}
        *q={};q->pool=p;q->index=i;q->generation=next();q->command=h;q->recording=c->generation;
    }
}
void LinuxVideoBoundary::hostResetQueries(VkQueryPool p,std::uint32_t first,std::uint32_t count){
    d_.hostResetQueries(device_,p,first,count);std::lock_guard<std::mutex> lock(gate_);
    for(auto& q:queries_)if(q.pool==p&&q.index>=first&&q.index-first<count)q={};
}
void LinuxVideoBoundary::beginQuery(VkCommandBuffer h,VkQueryPool p,std::uint32_t i,VkQueryControlFlags flags){
    d_.beginQuery(h,p,i,flags);std::lock_guard<std::mutex> lock(gate_);auto* c=command(h);auto* q=query(p,i);
    if(!c)return;
    if(!c->recording||!c->valid||!c->session||c->pool||flags||!q||q->command!=h||q->recording!=c->generation||!q->generation||q->region.serial){c->valid=false;return;}
    c->pool=p;c->query=i;
}
void LinuxVideoBoundary::encode(VkCommandBuffer h,const VkVideoEncodeInfoKHR* i){
    d_.encode(h,i);std::lock_guard<std::mutex> lock(gate_);auto* c=command(h);
    if(!c||!c->valid||!c->recording)return;
    auto* q=query(c->pool,c->query);auto* s=session(c->session);auto* v=i?view(i->srcPictureResource.imageViewBinding):nullptr;
    auto* p=i?find<VkVideoEncodeH265PictureInfoKHR>(i->pNext,VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR):nullptr;
    if(!i||!s||!v||!q||v->format!=s->format||q->region.serial||!p||!p->pStdPictureInfo||!p->naluSliceSegmentEntryCount||p->naluSliceSegmentEntryCount>512||!i->dstBuffer||!i->dstBufferRange||i->dstBufferRange>16*1024*1024||i->dstBufferOffset>std::numeric_limits<VkDeviceSize>::max()-i->dstBufferRange||!extent(i->srcPictureResource.codedExtent)||i->srcPictureResource.codedExtent.width>s->extent.width||i->srcPictureResource.codedExtent.height>s->extent.height||i->srcPictureResource.baseArrayLayer>=v->range.layerCount||v->range.baseArrayLayer>std::numeric_limits<std::uint32_t>::max()-i->srcPictureResource.baseArrayLayer){c->valid=false;return;}
    if(c->writtenPool){c->valid=false;return;}
    c->writtenPool=c->pool;c->writtenQuery=c->query;
    auto& r=q->region;r.serial=next();r.device=device_;r.session=s->handle;r.sessionGeneration=s->generation;r.view=v->handle;r.viewGeneration=v->generation;r.image=v->image;r.format=v->format;r.subresource=v->range;r.arrayLayer=v->range.baseArrayLayer+i->srcPictureResource.baseArrayLayer;r.codedOffset=i->srcPictureResource.codedOffset;r.extent=i->srcPictureResource.codedExtent;r.bitstream=i->dstBuffer;r.offset=i->dstBufferOffset;r.capacity=i->dstBufferRange;r.recordingGeneration=c->generation;r.pictureOrderCount=p->pStdPictureInfo->PicOrderCntVal;
}
void LinuxVideoBoundary::endQuery(VkCommandBuffer h,VkQueryPool p,std::uint32_t i){
    d_.endQuery(h,p,i);std::lock_guard<std::mutex> lock(gate_);auto* c=command(h);auto* q=query(p,i);
    if(!c)return;
    if(!c->valid||c->pool!=p||c->query!=i||!q||!q->region.serial)c->valid=false;else q->ended=true;
    c->pool=VK_NULL_HANDLE;
}
VkResult LinuxVideoBoundary::submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* input,VkFence fence){
    auto status=d_.submit(queue,count,input,fence);if(status!=VK_SUCCESS)return status;
    std::lock_guard<std::mutex> lock(gate_);
    // This first boundary recognizes one encode command per native submission.
    // An unrecognized write cannot leave an older completed query admissible.
    if(!fence||!input||count!=1||input->pNext||input->commandBufferCount!=1||!input->pCommandBuffers){for(auto& q:queries_)q={};return status;}
    auto* c=command(input->pCommandBuffers[0]);
    if(!c||!c->ended||!c->valid||!c->writtenPool){for(auto& q:queries_)q={};return status;}
    auto* q=query(c->writtenPool,c->writtenQuery);
    if(q&&(q->command!=c->handle||q->recording!=c->generation||q->submitted)){*q={};return status;}
    if(q&&q->ended&&q->region.serial){q->submitted=true;q->fence=fence;}
    return status;
}
VkResult LinuxVideoBoundary::fenceStatus(VkFence fence){
    auto status=d_.fenceStatus(device_,fence);if(status==VK_SUCCESS){std::lock_guard<std::mutex> lock(gate_);for(auto& q:queries_)if(q.fence==fence&&q.submitted)q.completed=true;}return status;
}
void LinuxVideoBoundary::retireFence(VkFence fence){for(auto& q:queries_)if(q.fence==fence&&fence)q={};}
VkResult LinuxVideoBoundary::resetFences(std::uint32_t count,const VkFence* fences){
    auto status=d_.resetFences(device_,count,fences);if(status==VK_SUCCESS){std::lock_guard<std::mutex> lock(gate_);if(!fences||count>Limit){for(auto& q:queries_)q={};}else for(std::uint32_t i=0;i<count;++i)retireFence(fences[i]);}return status;
}
void LinuxVideoBoundary::destroyFence(VkFence fence,const VkAllocationCallbacks* a){{std::lock_guard<std::mutex> lock(gate_);retireFence(fence);}d_.destroyFence(device_,fence,a);}
VkResult LinuxVideoBoundary::results(VkQueryPool p,std::uint32_t first,std::uint32_t count,std::size_t size,void* data,VkDeviceSize stride,VkQueryResultFlags flags,LinuxEncodedRegion& out){
    out={};auto status=d_.results(device_,p,first,count,size,data,stride,flags);
    constexpr auto allowed=VK_QUERY_RESULT_WAIT_BIT|VK_QUERY_RESULT_WITH_STATUS_BIT_KHR|VK_QUERY_RESULT_64_BIT;
    auto word=(flags&VK_QUERY_RESULT_64_BIT)?8u:4u;
    if(status!=VK_SUCCESS||count!=1||!data||!(flags&VK_QUERY_RESULT_WITH_STATUS_BIT_KHR)||(flags&~allowed)||size<word*3u||stride<word*3u)return status;
    auto read=[&](unsigned n){std::uint64_t value=0;std::memcpy(&value,static_cast<const std::uint8_t*>(data)+n*word,word);return value;};
    auto offset=read(0),bytes=read(1),completion=read(2);
    if(!completion||(word==4?completion>>31:completion>>63)||!bytes)return status;
    std::lock_guard<std::mutex> lock(gate_);auto* q=query(p,first);
    if(!q||!q->submitted||!q->completed||q->emitted||!current(q->region)||offset>q->region.capacity||bytes>q->region.capacity-offset)return status;
    out=q->region;out.offset+=offset;out.bytes=bytes;q->emitted=true;
    return status;
}
} // namespace stream_bridge::sender
