// Real public Vulkan ABI forwards with an explicit in-process native API fake.
// No Vulkan implementation, actual GPU encoding or Linux vrlink is executed.
#include "pc/stream-bridge-sender-linux-video.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
template<class T> T handle(std::uintptr_t n){if constexpr(std::is_pointer<T>::value)return reinterpret_cast<T>(n);else return static_cast<T>(n);}
constexpr VkResult failure=VK_ERROR_DEVICE_LOST;
struct Native {
    static unsigned calls;static VkResult submitResult,fenceResult,queryResult;
    static std::array<std::uint64_t,3> feedback;
    static VkResult VKAPI_CALL createSession(VkDevice,const VkVideoSessionCreateInfoKHR*,const VkAllocationCallbacks*,VkVideoSessionKHR* p){++calls;*p=handle<VkVideoSessionKHR>(10);return VK_SUCCESS;}
    static void VKAPI_CALL destroySession(VkDevice,VkVideoSessionKHR,const VkAllocationCallbacks*){++calls;}
    static VkResult VKAPI_CALL createPool(VkDevice,const VkQueryPoolCreateInfo*,const VkAllocationCallbacks*,VkQueryPool* p){++calls;*p=handle<VkQueryPool>(20);return VK_SUCCESS;}
    static void VKAPI_CALL destroyPool(VkDevice,VkQueryPool,const VkAllocationCallbacks*){++calls;}
    static VkResult VKAPI_CALL createView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* p){++calls;*p=handle<VkImageView>(30);return VK_SUCCESS;}
    static void VKAPI_CALL destroyView(VkDevice,VkImageView,const VkAllocationCallbacks*){++calls;}
    static void VKAPI_CALL destroyImage(VkDevice,VkImage,const VkAllocationCallbacks*){++calls;}
    static void VKAPI_CALL destroyBuffer(VkDevice,VkBuffer,const VkAllocationCallbacks*){++calls;}
    static VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){++calls;return VK_SUCCESS;}
    static VkResult VKAPI_CALL end(VkCommandBuffer){++calls;return VK_SUCCESS;}
    static void VKAPI_CALL beginVideo(VkCommandBuffer,const VkVideoBeginCodingInfoKHR*){++calls;}
    static void VKAPI_CALL resetQueries(VkCommandBuffer,VkQueryPool,std::uint32_t,std::uint32_t){++calls;}
    static void VKAPI_CALL hostResetQueries(VkDevice,VkQueryPool,std::uint32_t,std::uint32_t){++calls;}
    static void VKAPI_CALL beginQuery(VkCommandBuffer,VkQueryPool,std::uint32_t,VkQueryControlFlags){++calls;}
    static void VKAPI_CALL endQuery(VkCommandBuffer,VkQueryPool,std::uint32_t){++calls;}
    static void VKAPI_CALL encode(VkCommandBuffer,const VkVideoEncodeInfoKHR*){++calls;}
    static VkResult VKAPI_CALL submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence){++calls;return submitResult;}
    static VkResult VKAPI_CALL fenceStatus(VkDevice,VkFence){++calls;return fenceResult;}
    static VkResult VKAPI_CALL resetFences(VkDevice,std::uint32_t,const VkFence*){++calls;return VK_SUCCESS;}
    static void VKAPI_CALL destroyFence(VkDevice,VkFence,const VkAllocationCallbacks*){++calls;}
    static VkResult VKAPI_CALL results(VkDevice,VkQueryPool,std::uint32_t,std::uint32_t,std::size_t bytes,void* data,VkDeviceSize,VkQueryResultFlags flags){
        ++calls;const unsigned word=flags&VK_QUERY_RESULT_64_BIT?8:4;
        if(data&&bytes>=word*3)for(unsigned i=0;i<3;++i)std::memcpy(static_cast<unsigned char*>(data)+i*word,&feedback[i],word);
        return queryResult;
    }
    static LinuxVideoDispatch dispatch(){
        LinuxVideoDispatch d{};d.createSession=createSession;d.destroySession=destroySession;d.createPool=createPool;d.destroyPool=destroyPool;d.createView=createView;d.destroyView=destroyView;d.destroyImage=destroyImage;d.destroyBuffer=destroyBuffer;d.begin=begin;d.end=end;d.beginVideo=beginVideo;d.resetQueries=resetQueries;d.hostResetQueries=hostResetQueries;d.beginQuery=beginQuery;d.endQuery=endQuery;d.encode=encode;d.submit=submit;d.fenceStatus=fenceStatus;d.resetFences=resetFences;d.destroyFence=destroyFence;d.results=results;return d;
    }
};
unsigned Native::calls=0;VkResult Native::submitResult=VK_SUCCESS,Native::fenceResult=VK_NOT_READY,Native::queryResult=VK_SUCCESS;
std::array<std::uint64_t,3> Native::feedback{{32,96,1}};
struct Fixture {
    LinuxVideoBoundary boundary{handle<VkDevice>(1),Native::dispatch()};
    VkVideoSessionKHR session{};VkQueryPool pool{};VkImageView view{};
    VkCommandBuffer command=handle<VkCommandBuffer>(40);VkFence fence=handle<VkFence>(50);
    VkVideoEncodeH265ProfileInfoKHR codec{};VkVideoProfileInfoKHR profile{};
    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback{};VkQueryPoolCreateInfo queryInfo{};
    VkVideoSessionCreateInfoKHR sessionInfo{};VkImageViewCreateInfo viewInfo{};
    Fixture(){
        Native::submitResult=VK_SUCCESS;Native::fenceResult=VK_NOT_READY;Native::queryResult=VK_SUCCESS;Native::feedback={{32,96,1}};
        codec.sType=VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR;codec.stdProfileIdc=STD_VIDEO_H265_PROFILE_IDC_MAIN;
        profile.sType=VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;profile.pNext=&codec;profile.videoCodecOperation=VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        sessionInfo.sType=VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;sessionInfo.pVideoProfile=&profile;sessionInfo.maxCodedExtent={1920,1080};sessionInfo.pictureFormat=VK_FORMAT_R8G8B8A8_UNORM;
        check(boundary.createSession(&sessionInfo,nullptr,&session)==VK_SUCCESS,"actual session call forwarded");
        feedback.sType=VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;feedback.pNext=&profile;
        feedback.encodeFeedbackFlags=VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR|VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
        queryInfo.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;queryInfo.pNext=&feedback;queryInfo.queryType=VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;queryInfo.queryCount=4;
        check(boundary.createPool(&queryInfo,nullptr,&pool)==VK_SUCCESS,"actual query pool call forwarded");
        viewInfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;viewInfo.image=handle<VkImage>(60);viewInfo.format=VK_FORMAT_R8G8B8A8_UNORM;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,2,2};
        check(boundary.createView(&viewInfo,nullptr,&view)==VK_SUCCESS,"actual image view call forwarded");
    }
    void record(bool reset=true,bool duplicate=false){
        VkCommandBufferBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        check(boundary.begin(command,&b)==VK_SUCCESS,"native begin unchanged");
        VkVideoBeginCodingInfoKHR video{};video.sType=VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;video.videoSession=session;
        boundary.beginVideo(command,&video);
        if(reset)boundary.resetQueries(command,pool,0,1);
        boundary.beginQuery(command,pool,0,0);
        StdVideoEncodeH265PictureInfo stdPicture{};stdPicture.PicOrderCntVal=42;
        VkVideoEncodeH265NaluSliceSegmentInfoKHR slice{};slice.sType=VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR;
        VkVideoEncodeH265PictureInfoKHR h{};h.sType=VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR;h.pStdPictureInfo=&stdPicture;h.naluSliceSegmentEntryCount=1;h.pNaluSliceSegmentEntries=&slice;
        VkVideoEncodeInfoKHR encode{};encode.sType=VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;encode.pNext=&h;encode.dstBuffer=handle<VkBuffer>(70);encode.dstBufferOffset=128;encode.dstBufferRange=4096;
        encode.srcPictureResource.sType=VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;encode.srcPictureResource.imageViewBinding=view;encode.srcPictureResource.codedExtent={1024,512};encode.srcPictureResource.baseArrayLayer=1;
        boundary.encode(command,&encode);if(duplicate)boundary.encode(command,&encode);
        boundary.endQuery(command,pool,0);
        check(boundary.end(command)==VK_SUCCESS,"native end unchanged");
    }
    void submit(bool withFence=true){
        VkSubmitInfo i{};i.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;i.commandBufferCount=1;i.pCommandBuffers=&command;
        auto status=boundary.submit(handle<VkQueue>(80),1,&i,withFence?fence:VK_NULL_HANDLE);
        check(status==Native::submitResult,"original submit result unchanged");
    }
    void complete(){Native::fenceResult=VK_SUCCESS;check(boundary.fenceStatus(fence)==VK_SUCCESS,"actual successful fence call");}
    LinuxEncodedRegion read(VkQueryResultFlags flags=VK_QUERY_RESULT_WAIT_BIT|VK_QUERY_RESULT_WITH_STATUS_BIT_KHR,std::uint32_t first=0){
        std::array<std::uint64_t,3> data{};LinuxEncodedRegion out{};out.serial=999;
        auto before=Native::calls;
        auto status=boundary.results(pool,first,1,sizeof(data),data.data(),sizeof(data),flags,out);
        check(Native::calls==before+1,"exactly one original query, no injected poll or wait");
        check(status==Native::queryResult,"original query result unchanged");return out;
    }
};
void positive(){
    Fixture f;f.record();f.submit();check(!f.read().serial,"old ready query is not proof without exact submit fence");
    f.complete();auto r=f.read();check(r.serial&&r.image==handle<VkImage>(60)&&r.view==f.view&&r.format==VK_FORMAT_R8G8B8A8_UNORM,"real input view resolves to actual image");
    check(r.arrayLayer==3&&r.subresource.baseArrayLayer==2&&r.extent.width==1024,"original subresource and extent retained");
    check(r.bitstream==handle<VkBuffer>(70)&&r.offset==160&&r.bytes==96&&r.capacity==4096,"query range is relative to original encode buffer offset");
    check(r.pictureOrderCount==42&&r.sessionGeneration&&r.viewGeneration&&r.recordingGeneration,"native H265 picture and lifecycle generations retained");
    check(!f.read().serial,"query result cannot mint a second region");
    check(f.boundary.resetFences(1,&f.fence)==VK_SUCCESS,"native reset forwarded");
    f.record();f.submit();f.complete();auto second=f.read(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
    check(second.serial>r.serial,"same handles admit only fresh recording/query generation");
}
void reorderedRecordings(){
    Fixture f;f.record();auto older=f.command;
    f.command=handle<VkCommandBuffer>(41);f.record();f.submit();f.complete();
    // Both recordings legally reset/rewrite the same physical query slot.
    // Submitting the old recording last must not label its output as the newer one.
    f.command=older;f.fence=handle<VkFence>(51);f.submit();f.complete();
    check(!f.read().serial,"out-of-recording-order execution cannot alias a newer query record");
}
void refusals(){
    for(unsigned kind=0;kind<17;++kind){
        Fixture f;f.record(kind!=0,kind==1);
        if(kind==2)Native::submitResult=failure;
        f.submit(kind!=3);f.complete();
        if(kind==4)Native::queryResult=VK_NOT_READY;
        if(kind==5)Native::feedback[2]=std::uint32_t(-1);
        if(kind==6)Native::feedback[1]=4097;
        if(kind==7){f.boundary.destroyView(f.view,nullptr);check(f.boundary.createView(&f.viewInfo,nullptr,&f.view)==VK_SUCCESS,"view handle reused by actual native create");}
        if(kind==8){f.boundary.destroySession(f.session,nullptr);check(f.boundary.createSession(&f.sessionInfo,nullptr,&f.session)==VK_SUCCESS,"session handle reused by actual native create");}
        if(kind==9)check(f.boundary.resetFences(1,&f.fence)==VK_SUCCESS,"reset retires unconsumed fence evidence");
        if(kind==10)f.boundary.destroyImage(handle<VkImage>(60),nullptr);
        if(kind==11)f.boundary.destroyBuffer(handle<VkBuffer>(70),nullptr);
        if(kind==13)Native::feedback[2]=std::uint64_t(1)<<63;
        if(kind==14)f.boundary.destroyFence(f.fence,nullptr);
        if(kind==15)f.boundary.hostResetQueries(f.pool,0,1);
        if(kind==16)f.boundary.resetQueries(handle<VkCommandBuffer>(999),f.pool,0,1);
        auto r=f.read(kind==12?VK_QUERY_RESULT_WAIT_BIT:kind==13?VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_STATUS_BIT_KHR:VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
        check(!r.serial,"unknown, failed, stale, overwritten or ambiguous completion stays unproven");
    }
}
}
int main(){try{positive();refusals();reorderedRecordings();std::cout<<"Linux public Vulkan ABI: "<<checks<<" checks; no GPU/runtime/install authority claimed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
