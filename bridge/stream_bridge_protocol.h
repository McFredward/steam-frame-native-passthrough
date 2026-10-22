#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stream_bridge {
using Nonce = std::array<std::uint8_t,16>;
struct Bytes { const std::uint8_t* data=nullptr; std::size_t size=0; };
constexpr std::uint16_t ProtocolMajor=1, ProtocolMinor=0;
constexpr std::size_t MessageBytes=80;
constexpr std::uint64_t CapNegotiation=1, CapAnnexBSei=2,
    CapSourceAssociation=4, CapOpaqueFallback=8, CapExplicitCoverage=16,
    CapRleLz4=32, CapRleZstd=64;
// Native deployment requires the original six bits. Zstd is independently
// optional: older peers keep the original intersection and encoding route.
constexpr std::uint64_t KnownCapabilities=127;
constexpr std::uint64_t NativeRequiredCapabilities=63;
constexpr std::uint64_t RequiredCapabilities=CapNegotiation|CapAnnexBSei|CapOpaqueFallback;
enum class Role : std::uint8_t { Producer=1, Receiver=2 };
enum class Kind : std::uint8_t { Hello=1, Ack=2, Commit=3 };
enum class Status { Ok, NoMessage, Duplicate, InvalidArgument, Malformed,
    UnsupportedVersion, IncompatibleCapabilities, Unauthenticated,
    ChannelMismatch, RoleMismatch, SessionMismatch, PeerChanged, OutOfOrder };
struct Message {
    Kind kind=Kind::Hello; Role role=Role::Producer;
    std::uint16_t major=ProtocolMajor, minor=ProtocolMinor;
    std::uint64_t buildId=0, offered=RequiredCapabilities;
    Nonce senderNonce{}, peerNonce{};
    std::uint64_t selected=0, required=RequiredCapabilities;
};
// This is an explicit CALLER TRUST BOUNDARY, not a cryptographic authenticator.
// The caller must establish authentication and a fresh transport binding first.
struct VerifiedChannel { bool authenticated=false; Nonce binding{}; };
struct LocalConfig {
    Role role=Role::Producer; std::uint64_t buildId=0; Nonce nonce{};
    std::uint64_t offered=RequiredCapabilities, required=RequiredCapabilities;
};
bool operator==(const Message&,const Message&);
bool operator!=(const Message&,const Message&);
Status encode(const Message&,std::array<std::uint8_t,MessageBytes>&);
Status decode(Bytes,Message&);
class Handshake {
public:
    Status begin(const LocalConfig&,const VerifiedChannel&);
    void reset(); // Mandatory on actual transport loss/reconnect. No timer.
    Status next(Message&) const;
    // Call only after that exact message was handed to the authenticated route.
    Status markSent(const Message&,const VerifiedChannel&);
    Status receive(Bytes,const VerifiedChannel&);
    bool confirmed() const;
    bool configured() const { return configured_; }
    std::uint64_t negotiatedCapabilities() const;
    Nonce localNonce() const { return local_.nonce; }
    Nonce peerNonce() const { return peer_.senderNonce; }
    Role localRole() const { return local_.role; }
    bool channelMatches(const VerifiedChannel& value) const;
private:
    Status channel(const VerifiedChannel&) const;
    Message make(Kind) const;
    LocalConfig local_{}; VerifiedChannel channel_{}; Message peer_{};
    Nonce previousLocal_{},previousPeer_{};
    bool configured_=false, peerKnown_=false, helloSent_=false, ackSent_=false,
        ackReceived_=false, commitSent_=false, commitReceived_=false;
};
constexpr std::size_t ProbeBytes=80;
enum class ProbeKind : std::uint8_t { Probe=1, Echo=2 };
struct ProbeMessage {
    ProbeKind kind=ProbeKind::Probe;
    Nonce producerNonce{},receiverNonce{};
    std::uint64_t sequence=0;
    Nonce freshToken{};
};
Status encodeProbe(const ProbeMessage&,std::array<std::uint8_t,ProbeBytes>&);
Status decodeProbe(Bytes,ProbeMessage&);
Status validateProbe(const Handshake&,const ProbeMessage&,const VerifiedChannel&);
// Echo only after the receiver observed these exact bytes in an actual coded AU.
// The API validates identity, not that caller-owned AU/selected-surface evidence.
Status makeProbeEcho(const Handshake&,const ProbeMessage&,const VerifiedChannel&,ProbeMessage&);
class ProbeTracker {
public:
    Status begin(const Handshake&,std::uint64_t sequence,const Nonce& freshToken,ProbeMessage&);
    Status acceptEcho(const Handshake&,Bytes,const VerifiedChannel&);
    void reset();
    bool observed() const { return observed_; } // Transport roundtrip ONLY, never MR permission.
private:
    ProbeMessage pending_{};
    std::uint64_t lastSequence_=0;
    Nonce lastToken_{};
    bool pendingValid_=false, observed_=false;
};
} // namespace stream_bridge
