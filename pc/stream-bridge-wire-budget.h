#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace stream_bridge::sender {
// Rate policy only, never frame/image permission. Account for bytes added AFTER
// NVENC so a producer's video budget also has room for its lossless metadata.
// Fixed storage, no clocks, allocation, I/O or native encoder calls.
class WireBudget {
public:
    void reset() noexcept { authority_=0;sum_=0;count_=at_=0;samples_={}; }
    void observe(std::uint64_t authority,std::size_t original,std::size_t complete) noexcept {
        if(authority!=authority_){reset();authority_=authority;}
        if(!authority||!original||complete<original||complete>64*1024*1024)return;
        if(count_==samples_.size())sum_-=samples_[at_];else ++count_;
        samples_[at_]=static_cast<std::uint32_t>(complete-original);sum_+=samples_[at_];at_=(at_+1)%samples_.size();
    }
    std::uint32_t reserve(std::uint64_t authority,std::uint32_t fpsNum,std::uint32_t fpsDen)const noexcept {
        if(!authority||authority!=authority_||count_<8||!fpsDen||!fpsNum||fpsNum>1000000||fpsDen>1000000||fpsNum>std::uint64_t(fpsDen)*1000)return 0;
        // Ceiling avoids losing a bit every frame; include all owned framing
        // and HEVC emulation-prevention bytes, not just compressed mask bytes.
        const std::uint64_t divisor=std::uint64_t(count_)*fpsDen;
        const std::uint64_t bits=sum_*8*fpsNum;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>((bits+divisor-1)/divisor,UINT32_MAX));
    }
    static std::uint32_t videoRate(std::uint32_t requested,std::uint32_t metadata)noexcept {
        // Bounded quality floor. If metadata alone exceeds the budget, report
        // that pressure; never discard coverage or invent an opaque success.
        return requested-std::min(metadata,requested-(requested/4+(requested%4!=0)));
    }
private:
    std::uint64_t authority_=0,sum_=0;
    std::array<std::uint32_t,32> samples_{};
    std::size_t count_=0,at_=0;
};
}
