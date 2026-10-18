#pragma once
#include "stream-bridge-sender-provider-directmode.h"
#include "stream-bridge-sender-provider-core.h"
#include "bridge/stream_bridge_control.h"
#include <memory>
namespace stream_bridge::sender {
// Process-lifetime native GPU observer. Construct before publishing the NVENC
// factory; tick runs only on the cold provider worker. All refusals leave native
// video alone. ready is source-mode preparation, never selected-image proof.
class SenderCoverage {
public:
    SenderCoverage(DirectModeBoundary&,NvencBoundary&,ControlRuntime&);
    ~SenderCoverage();
    SenderCoverage(const SenderCoverage&)=delete;
    SenderCoverage& operator=(const SenderCoverage&)=delete;
    bool configure(std::uintptr_t verifiedNativeModule)noexcept;
    void tick()noexcept;
    void stop()noexcept;
    bool ready(std::uint32_t processId)const noexcept;
    bool operational(std::uint32_t processId)const noexcept;
    const char* stateName()const noexcept; // Bounded cold-worker status, no identifiers/secrets.
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge::sender
