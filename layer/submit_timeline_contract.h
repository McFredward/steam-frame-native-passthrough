#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace stream_bridge_submit {
enum class TimelineStatus : std::uint8_t { Accepted, Shape, Extension };
// Pure validation shared with the original audited draw transaction. Adding
// command buffers never changes the original semaphore counts or value arrays.
// No ownership or image/frame authority is conferred by a valid extension.
inline TimelineStatus canonicalTimeline(const VkSubmitInfo& info,std::uint32_t& nextType) noexcept {
    nextType=0;
    if(!info.pNext)return TimelineStatus::Accepted;
    const auto* base=static_cast<const VkBaseInStructure*>(info.pNext);
    nextType=static_cast<std::uint32_t>(base->sType);
    if(base->sType!=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO||base->pNext)return TimelineStatus::Extension;
    const auto* timeline=static_cast<const VkTimelineSemaphoreSubmitInfo*>(info.pNext);
    if(timeline->waitSemaphoreValueCount!=info.waitSemaphoreCount||timeline->signalSemaphoreValueCount!=info.signalSemaphoreCount||
       (timeline->waitSemaphoreValueCount&&!timeline->pWaitSemaphoreValues)||
       (timeline->signalSemaphoreValueCount&&!timeline->pSignalSemaphoreValues))return TimelineStatus::Shape;
    return TimelineStatus::Accepted;
}
}
