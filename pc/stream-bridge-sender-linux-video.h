#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <mutex>

namespace stream_bridge::sender {
// Public Vulkan encode completion evidence only. This is NOT an application
// source-format grant, a coverage certificate or an installable Linux backend.
struct LinuxEncodedRegion {
    std::uint64_t serial=0,sessionGeneration=0,viewGeneration=0,recordingGeneration=0;
    VkDevice device=VK_NULL_HANDLE;
    VkVideoSessionKHR session=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;VkFormat format=VK_FORMAT_UNDEFINED;
    VkImageSubresourceRange subresource{};std::uint32_t arrayLayer=0;
    VkOffset2D codedOffset{};VkExtent2D extent{};
    VkBuffer bitstream=VK_NULL_HANDLE;VkDeviceSize offset=0,bytes=0,capacity=0;
    std::int32_t pictureOrderCount=0;
};
struct LinuxVideoDispatch {
    PFN_vkCreateVideoSessionKHR createSession=nullptr;PFN_vkDestroyVideoSessionKHR destroySession=nullptr;
    PFN_vkCreateQueryPool createPool=nullptr;PFN_vkDestroyQueryPool destroyPool=nullptr;
    PFN_vkCreateImageView createView=nullptr;PFN_vkDestroyImageView destroyView=nullptr;
    PFN_vkDestroyImage destroyImage=nullptr;PFN_vkDestroyBuffer destroyBuffer=nullptr;
    PFN_vkBeginCommandBuffer begin=nullptr;PFN_vkEndCommandBuffer end=nullptr;
    PFN_vkCmdBeginVideoCodingKHR beginVideo=nullptr;PFN_vkCmdResetQueryPool resetQueries=nullptr;
    PFN_vkResetQueryPool hostResetQueries=nullptr;
    PFN_vkCmdBeginQuery beginQuery=nullptr;PFN_vkCmdEndQuery endQuery=nullptr;
    PFN_vkCmdEncodeVideoKHR encode=nullptr;PFN_vkQueueSubmit submit=nullptr;
    PFN_vkGetFenceStatus fenceStatus=nullptr;PFN_vkResetFences resetFences=nullptr;
    PFN_vkDestroyFence destroyFence=nullptr;PFN_vkGetQueryPoolResults results=nullptr;
};
// Per-device owner; native Vulkan's command/queue external synchronization still
// applies. Native calls always execute outside the bounded bookkeeping lock.
// No GPU waits, mappings, allocations, I/O or AU mutations are added here.
class LinuxVideoBoundary {
public:
    explicit LinuxVideoBoundary(VkDevice device,const LinuxVideoDispatch& dispatch):device_(device),d_(dispatch){}
    VkResult createSession(const VkVideoSessionCreateInfoKHR*,const VkAllocationCallbacks*,VkVideoSessionKHR*);
    void destroySession(VkVideoSessionKHR,const VkAllocationCallbacks*);
    VkResult createPool(const VkQueryPoolCreateInfo*,const VkAllocationCallbacks*,VkQueryPool*);
    void destroyPool(VkQueryPool,const VkAllocationCallbacks*);
    VkResult createView(const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView*);
    void destroyView(VkImageView,const VkAllocationCallbacks*);
    void destroyImage(VkImage,const VkAllocationCallbacks*);
    void destroyBuffer(VkBuffer,const VkAllocationCallbacks*);
    VkResult begin(VkCommandBuffer,const VkCommandBufferBeginInfo*);
    VkResult end(VkCommandBuffer);
    void beginVideo(VkCommandBuffer,const VkVideoBeginCodingInfoKHR*);
    void resetQueries(VkCommandBuffer,VkQueryPool,std::uint32_t first,std::uint32_t count);
    void hostResetQueries(VkQueryPool,std::uint32_t first,std::uint32_t count);
    void beginQuery(VkCommandBuffer,VkQueryPool,std::uint32_t,VkQueryControlFlags);
    void endQuery(VkCommandBuffer,VkQueryPool,std::uint32_t);
    void encode(VkCommandBuffer,const VkVideoEncodeInfoKHR*);
    VkResult submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
    VkResult fenceStatus(VkFence);
    VkResult resetFences(std::uint32_t,const VkFence*);
    void destroyFence(VkFence,const VkAllocationCallbacks*);
    // out is cleared on every call; a native successful return is never changed.
    // A region is emitted once, only after the exact submit's fence completed.
    VkResult results(VkQueryPool,std::uint32_t first,std::uint32_t count,
                     std::size_t,void*,VkDeviceSize,VkQueryResultFlags,LinuxEncodedRegion& out);
private:
    static constexpr std::size_t Limit=32;
    struct Session {VkVideoSessionKHR handle=VK_NULL_HANDLE;std::uint64_t generation=0;VkExtent2D extent{};VkFormat format=VK_FORMAT_UNDEFINED;};
    struct View {VkImageView handle=VK_NULL_HANDLE;std::uint64_t generation=0;VkImage image=VK_NULL_HANDLE;VkImageSubresourceRange range{};VkFormat format=VK_FORMAT_UNDEFINED;};
    struct Pool {VkQueryPool handle=VK_NULL_HANDLE;std::uint64_t generation=0;std::uint32_t count=0;};
    struct Query {VkQueryPool pool=VK_NULL_HANDLE;std::uint32_t index=0;std::uint64_t generation=0,recording=0;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;bool ended=false,submitted=false,completed=false,emitted=false;LinuxEncodedRegion region{};};
    struct Command {VkCommandBuffer handle=VK_NULL_HANDLE;std::uint64_t generation=0;VkVideoSessionKHR session=VK_NULL_HANDLE;VkQueryPool pool=VK_NULL_HANDLE,writtenPool=VK_NULL_HANDLE;std::uint32_t query=0,writtenQuery=0;bool recording=false,ended=false,valid=true;};
    Session* session(VkVideoSessionKHR);View* view(VkImageView);Pool* pool(VkQueryPool);
    Command* command(VkCommandBuffer);Query* query(VkQueryPool,std::uint32_t);
    void retireFence(VkFence);bool current(const LinuxEncodedRegion&)const;
    std::uint64_t next();
    VkDevice device_;LinuxVideoDispatch d_;std::mutex gate_;
    std::uint64_t generation_=0;
    std::array<Session,Limit> sessions_{};std::array<View,Limit> views_{};
    std::array<Pool,Limit> pools_{};std::array<Command,Limit> commands_{};
    std::array<Query,Limit> queries_{};
};
} // namespace stream_bridge::sender
