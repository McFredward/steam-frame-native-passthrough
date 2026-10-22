#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace stream_bridge::sender {
// Source-copy, NVENC and cold-resource ownership are independent of the render
// metadata mutex. A CPU readback of one accepted input must not discard a copy,
// binding or retirement belonging to another input. Each claim is bounded and
// generation tagged; no callback waits for another owner to finish.
template<class Resources, std::size_t Capacity=4> class CoverageStages {
public:
    enum class Phase : std::uint64_t { Free, Writing, Captured, Binding, Bound, Reading, Retired, Maintenance };
    struct Receipt {
        std::uintptr_t input=0;
        std::uint64_t contract=0,present=0,copyMicros=0,submissionMicros=0,submission=0,encoderGeneration=0;
        void* encoder=nullptr;
        std::uint32_t pid=0,width=0,height=0;
        bool ordinaryVideo=false;
    };
    struct Entry {Resources resources{};Receipt receipt{};};
private:
    static constexpr std::uint64_t PhaseMask=15,GenerationStep=16;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"coverage ownership cannot wait on an atomic-library lock");
    static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,"coverage identity lookup cannot wait");
    struct Slot {std::atomic<std::uint64_t> word{0};std::atomic<std::uintptr_t> input{0};Entry entry{};};
public:
    class Claim {
    public:
        Claim()=default;
        Claim(const Claim&)=delete;Claim& operator=(const Claim&)=delete;
        Claim(Claim&& other)noexcept{swap(other);}
        Claim& operator=(Claim&& other)noexcept{if(this!=&other){reset();swap(other);}return *this;}
        ~Claim(){reset();}
        explicit operator bool()const noexcept{return slot_!=nullptr;}
        Entry* operator->()const noexcept{return &slot_->entry;}
        Entry& operator*()const noexcept{return slot_->entry;}
        std::size_t index()const noexcept{return index_;}
        // Immutable metadata is complete before the Release publication. A
        // claimant owns the whole Entry until it publishes or leaves scope.
        void publish(Phase phase)noexcept{
            if(!slot_)return;
            slot_->input.store(phase==Phase::Captured?slot_->entry.receipt.input:0,std::memory_order_relaxed);
            slot_->word.store((word_&~PhaseMask)|static_cast<std::uint64_t>(phase),std::memory_order_release);
            slot_=nullptr;
        }
    private:
        friend class CoverageStages;
        Claim(Slot* slot,std::uint64_t word,Phase previous,std::size_t index)noexcept:slot_(slot),word_(word),previous_(previous),index_(index){}
        void reset()noexcept{publish(previous_);}
        void swap(Claim& other)noexcept{std::swap(slot_,other.slot_);std::swap(word_,other.word_);std::swap(previous_,other.previous_);std::swap(index_,other.index_);}
        Slot* slot_=nullptr;std::uint64_t word_=0;Phase previous_=Phase::Free;std::size_t index_=0;
    };
    using PoolClaim=std::array<Claim,Capacity>;
    Claim capture(std::uintptr_t input)noexcept{
        if(!input)return {};
        // One native producer serializes colour CopyResource. Replace only an
        // unconsumed receipt for that exact canonical input; never a Bound one.
        for(std::size_t i=0;i<Capacity;++i)if(matchesCaptured(i,input)){
            auto result=claim(i,Phase::Captured,Phase::Writing);if(result)return result;
        }
        for(std::size_t i=0;i<Capacity;++i){
            auto result=claim(i,Phase::Free,Phase::Writing);if(result)return result;
            result=claim(i,Phase::Retired,Phase::Writing);if(result)return result;
        }
        return {};
    }
    Claim discardCaptured(std::uintptr_t input)noexcept{
        if(!input)return {};
        for(std::size_t i=0;i<Capacity;++i)if(matchesCaptured(i,input)){
            auto owned=claim(i,Phase::Captured,Phase::Writing);if(owned)return owned;
        }
        return {};
    }
    bool bind(std::uintptr_t input,std::uint32_t width,std::uint32_t height,
              std::uint64_t submission,void* encoder,std::uint64_t encoderGeneration,std::uint64_t micros)noexcept{
        if(!input||!submission||!encoder||!encoderGeneration)return false;
        for(std::size_t i=0;i<Capacity;++i)if(matchesCaptured(i,input)){
            auto owned=claim(i,Phase::Captured,Phase::Binding);if(!owned)continue;
            auto& r=owned->receipt;
            if(r.input!=input||r.width!=width||r.height!=height||!r.present||!r.contract)return false;
            r.submission=submission;r.encoder=encoder;r.encoderGeneration=encoderGeneration;r.submissionMicros=micros;
            owned.publish(Phase::Bound);return true;
        }
        return false;
    }
    Claim read(std::uint64_t submission,void* encoder,std::uint64_t encoderGeneration)noexcept{
        if(!submission||!encoder||!encoderGeneration)return {};
        // Actual NVENC callbacks are serialized by NvencBoundary. No other
        // reader/retirer can own a Bound entry while this callback is running.
        for(std::size_t i=0;i<Capacity;++i){
            auto owned=claim(i,Phase::Bound,Phase::Reading);if(!owned)continue;
            const auto& r=owned->receipt;
            if(r.submission==submission&&r.encoder==encoder&&r.encoderGeneration==encoderGeneration)return owned;
        }
        return {};
    }
    bool maintenance(PoolClaim& all)noexcept{
        PoolClaim pending;
        for(std::size_t i=0;i<Capacity;++i){
            for(auto phase:{Phase::Free,Phase::Captured,Phase::Retired}){
                pending[i]=claim(i,phase,Phase::Maintenance);if(pending[i])break;
            }
            if(!pending[i])return false; // RAII restores partial claims verbatim.
        }
        all=std::move(pending);return true;
    }
private:
    bool matchesCaptured(std::size_t i,std::uintptr_t input)const noexcept{
        // Acquire the Release-published phase before inspecting its identity.
        // Reading the key first could miss a newly published handoff.
        const auto& slot=slots_[i];return (slot.word.load(std::memory_order_acquire)&PhaseMask)==static_cast<std::uint64_t>(Phase::Captured)&&slot.input.load(std::memory_order_relaxed)==input;
    }
    Claim claim(std::size_t i,Phase expected,Phase owned)noexcept{
        auto& slot=slots_[i];auto word=slot.word.load(std::memory_order_acquire);
        if((word&PhaseMask)!=static_cast<std::uint64_t>(expected)||word>std::numeric_limits<std::uint64_t>::max()-GenerationStep)return {};
        const auto next=((word&~PhaseMask)+GenerationStep)|static_cast<std::uint64_t>(owned);
        if(!slot.word.compare_exchange_strong(word,next,std::memory_order_acquire,std::memory_order_relaxed))return {};
        return Claim(&slot,next,expected,i);
    }
    std::array<Slot,Capacity> slots_{};
};
} // namespace stream_bridge::sender
