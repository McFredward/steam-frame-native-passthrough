#pragma once
#include "stream_bridge_protocol.h"
#include <array>
#include <vector>

namespace stream_bridge {
constexpr std::size_t MaximumAccessUnitBytes=16*1024*1024;
constexpr std::size_t MaximumOwnedPayloadBytes=256;
constexpr std::size_t MaximumNalCount=4096;
constexpr std::size_t MaximumSeiBytes=64*1024;
constexpr std::array<std::uint8_t,16> BridgeUuid{{
    0x53,0x46,0x4e,0x50,0x2d,0x42,0x52,0x49,
    0x44,0x47,0x45,0x2d,0x56,0x31,0x00,0x01}};
enum class SeiStatus { Ok, NotFound, InvalidArgument, Malformed,
    LimitExceeded, DuplicateOwned, MixedOwned, NoVcl };
// Complete caller-owned Annex-B AU only. No bytes are modified in place.
// Failure leaves output unchanged; foreign SEI/VCL bytes are never rewritten.
SeiStatus inspectOwned(Bytes,std::vector<std::uint8_t>& payload);
SeiStatus appendOwned(Bytes accessUnit,Bytes payload,std::vector<std::uint8_t>& output);
SeiStatus removeOwned(Bytes accessUnit,std::vector<std::uint8_t>& output);
} // namespace stream_bridge
