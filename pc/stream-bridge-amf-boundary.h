#pragma once
// Public AMD AMF SDK interfaces are an explicit build input, not vendored here.
#include <components/Component.h>
#include <core/Buffer.h>
#include <array>
#include <cstdint>
#include <mutex>

namespace stream_bridge::sender {
// This is a public-ABI foundation, NOT an installed Steam/AMF hook. A production
// owner must first prove the actual factory, HEVC frame output, composition,
// coverage source and final native TX handoff for its pinned runtime.
struct AmfContract {
    bool explicitlyEnabled=false, pinnedNativeFactory=false, hevcComponent=false;
    bool completeAnnexBFrames=false;
    std::uint32_t width=0,height=0;
    // Fresh owned nonce for THIS attachment; never reuse across boundaries or
    // reloads while any old native output may remain. Not a video timestamp.
    std::array<std::int64_t,2> propertyDomain{};
};
struct AmfInputIdentity {
    std::uintptr_t nativeResource=0;
    std::uint64_t sourceGeneration=0,contentGeneration=0;
    std::uint32_t width=0,height=0;
    // An input resource/PTS alone never establishes scene coverage.
    bool exactSourceAndCoverage=false;
};
struct AmfOutputIdentity {
    std::uint64_t encoderGeneration=0,submission=0;
    AmfInputIdentity input{};
    amf::AMFData* inputObject=nullptr; // Borrowed, retained through observer call.
    amf::AMFBuffer* output=nullptr;   // Borrowed, retained through observer call.
};
struct AmfOriginals {
    AMF_RESULT (AMF_STD_CALL *submit)(amf::AMFComponent*,amf::AMFData*)=nullptr;
    AMF_RESULT (AMF_STD_CALL *query)(amf::AMFComponent*,amf::AMFData**)=nullptr;
    AMF_RESULT (AMF_STD_CALL *flush)(amf::AMFComponent*)=nullptr;
    AMF_RESULT (AMF_STD_CALL *reinit)(amf::AMFComponent*,amf_int32,amf_int32)=nullptr;
    AMF_RESULT (AMF_STD_CALL *terminate)(amf::AMFComponent*)=nullptr;
};
struct AmfObserver {
    // No reentry, waits or network/file work. Native AU and caller pointer are
    // never replaced by this foundation. Output may precede SubmitInput return:
    // in that case observation waits for the successful acceptance result.
    void (*output)(const AmfOutputIdentity&,void*) noexcept=nullptr;
    void* context=nullptr;
};
class AmfBoundary {
public:
    static constexpr std::size_t Capacity=32;
    AmfBoundary()=default;
    AmfBoundary(const AmfBoundary&)=delete;
    AmfBoundary& operator=(const AmfBoundary&)=delete;
    ~AmfBoundary(); // Caller has stopped/joined native callback users first.
    bool prepare(amf::AMFComponent*,AmfOriginals,const AmfContract&,AmfObserver={}) noexcept;
    AMF_RESULT submit(amf::AMFData*,const AmfInputIdentity&) noexcept;
    AMF_RESULT query(amf::AMFData**) noexcept;
    AMF_RESULT flush() noexcept;
    AMF_RESULT reinit(amf_int32,amf_int32) noexcept;
    AMF_RESULT terminate() noexcept;
    void stop() noexcept; // Retires receipts; forwarding remains available.
    std::size_t pending() const noexcept;
    bool active() const noexcept;
private:
    struct Entry {
        AmfOutputIdentity identity{};
        bool accepted=false;
        amf::AMFBuffer* earlyOutput=nullptr;
    };
    mutable std::mutex mutex_;
    amf::AMFComponent* component_=nullptr;
    AmfOriginals original_{};AmfContract contract_{};AmfObserver observer_{};
    std::array<Entry,Capacity> entries_{};
    std::uint64_t generation_=0,nextSubmission_=0;
    bool enabled_=false;
    void retire(Entry&) noexcept;
    void retireAll() noexcept;
    void publish(Entry&,amf::AMFBuffer*) noexcept;
};
} // namespace stream_bridge::sender
