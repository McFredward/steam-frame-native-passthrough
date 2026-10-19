#pragma once
#include "stream_bridge_protocol.h"

namespace stream_bridge {
constexpr std::size_t AuthHeaderBytes=64, AuthTagBytes=32;
constexpr std::size_t AuthEnvelopeBytes=AuthHeaderBytes+MessageBytes+AuthTagBytes;
using AuthEnvelope=std::array<std::uint8_t,AuthEnvelopeBytes>;
using AuthPayload=std::array<std::uint8_t,MessageBytes>;
using Secret=std::array<std::uint8_t,32>;
using AuthTag=std::array<std::uint8_t,AuthTagBytes>;
struct Pairing { Nonce pairId{}; Secret key{}; Role role=Role::Producer; };
enum class AuthStatus { Ok, InvalidArgument, NotPrepared, NotBound,
    Malformed, UnsupportedVersion, PairMismatch, RoleMismatch, BindingMismatch,
    Replay, BadMac, PayloadInvalid, SourceUnproved, AlreadyBound, SequenceExhausted };

// These identities are supplied by the real native-stream provider, not from
// a packet. This library cannot discover a connection or observe a coded AU.
struct VideoStreamContext { Nonce connection{}; std::uint64_t generation=0; };
struct VideoSourceEvidence {
    VideoStreamContext stream{};
    bool currentCompleteAccessUnit=false;
};

// Local signing capability ONLY: suitable for Handshake.begin/markSent after
// preparing the paired producer. It says nothing about a responding peer.
// Incoming data must always pass bootstrapFromVideo/openControl separately.
struct LocalSendAuthority { VerifiedChannel channel{}; };

// RFC 2104 HMAC using the existing SHA-256 primitive. Public only for vectors
// and bounded control authentication; key<=1024 B, data<=4096 B. Transactional.
AuthStatus hmacSha256(Bytes key,Bytes data,AuthTag& output) noexcept;
// Volatile stores for live key material owned by this module. No persistence.
void secureZero(void* data,std::size_t size) noexcept;

class AuthSession {
public:
    AuthSession()=default;
    ~AuthSession();
    AuthSession(const AuthSession&)=delete;
    AuthSession& operator=(const AuthSession&)=delete;
    AuthSession(AuthSession&&)=delete;
    AuthSession& operator=(AuthSession&&)=delete;

    AuthStatus prepareProducer(const Pairing&,const Nonce& freshBinding) noexcept;
    AuthStatus prepareReceiver(const Pairing&,const VideoStreamContext&) noexcept;
    // Mandatory on native-stream/control transport loss; no timeout/expiry.
    void reset() noexcept;
    AuthStatus localSendAuthority(LocalSendAuthority&) const noexcept;
    // Only known 80-byte SFB1/SFP1 messages with the local role can be signed.
    // Signing advances a distinct local counter; never grants peer authority.
    AuthStatus seal(Bytes payload,AuthEnvelope&) noexcept;
    // Receiver-only first message from the exact current complete coded AU.
    // Only a MAC-valid producer HELLO may establish a new binding.
    AuthStatus bootstrapFromVideo(Bytes envelope,const VideoSourceEvidence&,
        AuthPayload&,VerifiedChannel&) noexcept;
    // Bound control channel: authenticate every packet before use by handshake
    // or probe code. A raw TCP payload can never bootstrap a receiver.
    AuthStatus openControl(Bytes envelope,AuthPayload&,VerifiedChannel&) noexcept;
    // Domain-separated exact-AU certificates. Validation does not consume a
    // control sequence, bootstrap a channel or grant any image permission.
    AuthStatus signFrameHeader(Bytes,AuthTag&) const noexcept;
    AuthStatus verifyFrameHeader(Bytes header,Bytes tag,Nonce& binding) const noexcept;
    AuthStatus verifyInitialVideoHello(Bytes envelope,const Nonce& certifiedBinding) const noexcept;
    Nonce pairId() const noexcept { return pairId_; }
    bool prepared() const noexcept { return prepared_; }
    bool bound() const noexcept { return bound_; }
    bool peerVerified() const noexcept { return peerVerified_; }
    std::uint64_t sentSequence() const noexcept { return sent_; }
    std::uint64_t receivedSequence() const noexcept { return received_; }
    Nonce binding() const noexcept { return binding_; }
private:
    AuthStatus inspect(Bytes,const Nonce* expectedBinding,AuthPayload&,
        Nonce& binding,std::uint64_t& sequence,bool initialHello) const noexcept;
    Secret key_{};
    Nonce pairId_{},binding_{},previousPair_{},previousBinding_{};
    VideoStreamContext source_{};
    Role role_=Role::Producer;
    std::uint64_t sent_=0,received_=0;
    bool prepared_=false,bound_=false,peerVerified_=false;
};
} // namespace stream_bridge
