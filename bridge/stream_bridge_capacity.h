#pragma once
#include "stream_bridge_auth.h"
namespace stream_bridge {
// Before altering a coded AU, the paired receiver attests its ACTUAL native
// decode-input capacity. This admission does not bind video or grant coverage.
// Fresh producer binding+challenge defeat replay after a new connection.
struct CapacityMessage {
    Role role=Role::Producer;
    Nonce binding{},challenge{};
    VideoStreamContext source{};
    std::uint32_t maximumAccessUnitBytes=0;
};
class CapacityProtection {
public:
    ~CapacityProtection();
    bool prepare(const Pairing&) noexcept;
    void reset() noexcept;
    bool seal(const CapacityMessage&,AuthEnvelope&) const noexcept;
    bool open(Bytes,CapacityMessage&) const noexcept;
private:
    Pairing pair_{};bool prepared_=false;
};
} // namespace stream_bridge
