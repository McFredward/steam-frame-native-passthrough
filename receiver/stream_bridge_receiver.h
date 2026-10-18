#pragma once

#include "bridge/hevc_bridge_sei.h"
#include "../mask/frame_association.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace stream_bridge_receiver {

// All methods are serialized on a non-render worker. pollNotice copies a small
// value for a caller-owned UI queue; it must not call a UI from a decode hook.
// This adapter installs no hook, opens no transport and changes no video bytes.
using Session = mr::mask::SessionId;
using NativeFrame = mr::mask::NativeFrameIdentity;
using Surface = mr::mask::FrameResourceIdentity;

struct Authority {
    Session session{};
    std::uint64_t streamGeneration = 0;
    stream_bridge::VerifiedChannel channel{};
};

struct AccessUnitContext {
    Authority authority{};
    NativeFrame native{};
    // Caller trust boundaries, false by default. Establish these from the
    // admitted loaded provider, actual mapped buffer and native submission.
    bool sourceBoundaryVerified = false;
    bool completeAnnexBAccessUnit = false;
};

struct DecodedContext {
    Authority authority{};
    NativeFrame native{};
    Surface surface{};
    // Applies also to ordinary/non-probe surfaces: reuse must invalidate an old
    // probe before that reused surface could be selected. Never infer this bit.
    bool decodedAssociationVerified = false;
};

struct SelectedContext {
    Authority authority{};
    NativeFrame native{};
    Surface surface{};
    bool selectedAssociationVerified = false;
};

enum class Result : std::uint8_t {
    Ordinary, Handshake, MetadataObserved, Duplicate, UnverifiedBoundary,
    InvalidOwned, Incompatible, WrongSession, NotConfirmed, UnknownFrame,
    InvalidEvidence, StaleEvidence, CapacityExceeded, EvidenceLost
};

struct AccessUnitResult {
    stream_bridge::Bytes original{}; // Exact caller pointer and length, always.
    Result result = Result::Ordinary;
};

// Verify the exact owned payload from this actual AU once. Authentication must
// bind its native stream and may bootstrap only a producer HELLO from video.
// A callback success is a caller-established trust boundary, not a network
// assertion. It supplies the fixed plaintext and verified binding together.
using PayloadVerifier = bool (*)(stream_bridge::Bytes, const AccessUnitContext&,
    std::array<std::uint8_t, stream_bridge::MessageBytes>&,
    stream_bridge::VerifiedChannel&, void*) noexcept;

enum class NoticeCode : std::uint8_t {
    SenderMissing, SenderIncompatible, Negotiating, PeerConfirmed,
    MetadataObserved, MetadataSelected, EvidenceLost
};
struct Notice {
    NoticeCode code = NoticeCode::SenderMissing;
    Session session{};
    std::uint64_t streamGeneration = 0;
};

struct Snapshot {
    bool active = false;
    bool peerConfirmed = false;
    bool metadataObserved = false;
    bool metadataSelected = false;
    bool evidenceHealthy = false;
    std::size_t probeCount = 0;
};

// Tiny metadata only. A successful probe/selection is NOT image permission:
// there is no mask, shader, descriptor or MR-ready/renderer-activation API here.
// Resource identity is native content generation, not merely an allocation.
// No implicit expiry, guessed key epoch, latest-frame fallback or eviction.
// The caller must begin with fresh session/nonces after an actual reconnect.
class Receiver {
public:
    static constexpr std::size_t ProbeCapacity = 32;
    static constexpr std::size_t SurfaceCapacity = 64;

    bool begin(const Authority&, const stream_bridge::LocalConfig& receiver) noexcept;
    // Fresh native stream with no claimed peer channel yet. No outbound message
    // can be obtained until the first actual video HELLO passes the verifier.
    bool beginAuthenticated(const Authority&, const stream_bridge::LocalConfig& receiver,
        PayloadVerifier, void* verifierContext) noexcept;
    void end() noexcept;
    void evidenceLost() noexcept;
    AccessUnitResult observeAccessUnit(stream_bridge::Bytes, const AccessUnitContext&) noexcept;
    stream_bridge::Status receiveControl(stream_bridge::Bytes, const Authority&) noexcept;

    stream_bridge::Status nextOutbound(stream_bridge::Message&) const noexcept;
    stream_bridge::Status markOutboundSent(const stream_bridge::Message&, const Authority&) noexcept;
    bool nextEcho(stream_bridge::ProbeMessage&) const noexcept;
    bool markEchoSent(const stream_bridge::ProbeMessage&, const Authority&) noexcept;

    Result observeDecoded(const DecodedContext&) noexcept;
    std::optional<stream_bridge::ProbeMessage> selectedMetadata(const SelectedContext&) noexcept;

    // Explicit attempted-feature status, never per-frame toasts or a timer.
    void requestStatus() noexcept;
    bool pollNotice(Notice&) noexcept;
    Snapshot snapshot() const noexcept;

private:
    struct ProbeEntry {
        bool used = false, echoPending = false;
        NativeFrame native{};
        stream_bridge::ProbeMessage message{}, echo{};
    };
    struct SurfaceEntry {
        bool used = false;
        NativeFrame native{};
        Surface surface{};
    };
    bool matches(const Authority&) const noexcept;
    bool matchesStream(const Authority&) const noexcept;
    void notice(NoticeCode) noexcept;
    Result recordProbe(const stream_bridge::ProbeMessage&, NativeFrame) noexcept;
    ProbeEntry* find(NativeFrame) noexcept;
    const ProbeEntry* find(NativeFrame) const noexcept;
    Authority authority_{};
    Session previousSession_{};
    std::uint64_t lastGeneration_ = 0;
    bool everBegan_ = false, active_ = false, healthy_ = false;
    bool peerSeen_ = false, incompatible_ = false, observed_ = false, selected_ = false;
    std::uint16_t emitted_ = 0, pending_ = 0;
    stream_bridge::Handshake handshake_{};
    stream_bridge::LocalConfig authenticatedLocal_{};
    PayloadVerifier verifier_ = nullptr;
    void* verifierContext_ = nullptr;
    std::array<ProbeEntry, ProbeCapacity> probes_{};
    std::array<SurfaceEntry, SurfaceCapacity> surfaces_{};
};

} // namespace stream_bridge_receiver
