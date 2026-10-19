#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace stream_bridge::sender {
// Format permission comes from the kernel-validated live local application,
// never from finding a translucent pixel. This is NOT video/frame authority.
// One cold owner publishes; native callbacks only read one lock-free identity.
class SourceContract {
public:
    explicit SourceContract(std::uint32_t generation=0) noexcept:generation_(generation){}
    bool publish(std::uint32_t pid) noexcept {
        const auto previous=identity_.load(std::memory_order_acquire);
        if(static_cast<std::uint32_t>(previous)==pid)return true;
        if(generation_==std::numeric_limits<std::uint32_t>::max()){
            identity_.store(0,std::memory_order_release);return false;
        }
        ++generation_;
        identity_.store(pid?(std::uint64_t(generation_)<<32)|pid:0,std::memory_order_release);
        return true;
    }
    std::uint64_t snapshot(std::uint32_t pid) const noexcept {
        const auto value=identity_.load(std::memory_order_acquire);
        return pid&&static_cast<std::uint32_t>(value)==pid?value:0;
    }
    bool current(std::uint64_t receipt,std::uint32_t pid) const noexcept {
        return receipt&&snapshot(pid)==receipt;
    }
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"native source contract must not lock");
    std::atomic<std::uint64_t> identity_{0};std::uint32_t generation_=0;
};
} // namespace stream_bridge::sender
