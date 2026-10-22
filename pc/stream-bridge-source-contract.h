#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace stream_bridge::sender {
enum class SourceContractStatus : unsigned { Current, NoDeclaration, DifferentProcess, RevokedGeneration, Count };
struct SourceContractObservation {
    SourceContractStatus status=SourceContractStatus::NoDeclaration;
    std::uint32_t declaredProcess=0,nativeProcess=0;
    bool current()const noexcept{return status==SourceContractStatus::Current;}
};
// Format permission comes from the kernel-validated live local application,
// never from finding a translucent pixel. This is NOT video/frame authority.
// One cold owner publishes; native callbacks only read one lock-free identity.
class SourceContract {
public:
    explicit SourceContract(std::uint32_t generation=0) noexcept:generation_(generation){}
    bool publish(std::uint32_t pid,bool metadataBudget=false) noexcept {
        const auto previous=identity_.load(std::memory_order_acquire);
        if(static_cast<std::uint32_t>(previous)==pid){budget_.store(pid&&metadataBudget?previous:0,std::memory_order_release);return true;}
        if(generation_==std::numeric_limits<std::uint32_t>::max()){
            budget_.store(0,std::memory_order_release);identity_.store(0,std::memory_order_release);return false;
        }
        ++generation_;
        const auto next=pid?(std::uint64_t(generation_)<<32)|pid:0;
        budget_.store(metadataBudget?next:0,std::memory_order_release);
        identity_.store(next,std::memory_order_release);
        return true;
    }
    std::uint64_t snapshot(std::uint32_t pid) const noexcept {
        const auto value=identity_.load(std::memory_order_acquire);
        return pid&&static_cast<std::uint32_t>(value)==pid?value:0;
    }
    std::uint64_t budgetAuthority(std::uint32_t pid)const noexcept {
        const auto value=snapshot(pid);return value&&budget_.load(std::memory_order_acquire)==value?value:0;
    }
    // Optional codec policy only. Never authorizes a picture or mask association.
    // Every live explicit-RGBA source can use a mutually negotiated lossless codec.
    bool entropyCodecAllowed(std::uint64_t receipt,std::uint32_t pid,bool negotiated)const noexcept {
        return receipt&&negotiated&&current(receipt,pid);
    }
    std::uint64_t cameraAuthority()const noexcept{return identity_.load(std::memory_order_acquire);}
    bool current(std::uint64_t receipt,std::uint32_t pid) const noexcept {
        return inspect(receipt,pid).current();
    }
    SourceContractObservation inspect(std::uint64_t receipt,std::uint32_t pid) const noexcept {
        // One coherent atomic observation both decides the original permission
        // and explains a refusal. A second diagnostic load could instead name a
        // later declaration and misattribute a loading/source transition.
        const auto value=identity_.load(std::memory_order_acquire);
        SourceContractObservation out{SourceContractStatus::NoDeclaration,static_cast<std::uint32_t>(value),pid};
        if(value){
            if(!pid||out.declaredProcess!=pid)out.status=SourceContractStatus::DifferentProcess;
            // A zero capture receipt never held a declaration. Reserve the
            // revoked-generation reason for a genuinely captured old lease.
            else if(!receipt)out.status=SourceContractStatus::NoDeclaration;
            else if(receipt!=value)out.status=SourceContractStatus::RevokedGeneration;
            else out.status=SourceContractStatus::Current;
        }
        return out;
    }
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"native source contract must not lock");
    std::atomic<std::uint64_t> identity_{0},budget_{0};std::uint32_t generation_=0;
};
} // namespace stream_bridge::sender
