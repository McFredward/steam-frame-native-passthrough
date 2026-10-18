#pragma once
#include "bridge/stream_bridge_protocol.h"
#include "bridge/hevc_bridge_sei.h"
#include <array>
#include <cstdint>
#include <vector>

namespace stream_bridge::sender {
// Caller trust boundary, NOT evidence manufactured by this portable adapter.
// A future live bootstrap must establish the exact loaded module, public ABI,
// encoder generation and ordinary complete HEVC AU before setting verified.
struct VerifiedSource {
    bool verified=false, ordinaryStream=false, completeAccessUnit=false;
    enum class Codec { Unknown, H264, HevcAnnexB, Av1 } codec=Codec::Unknown;
    std::array<std::uint8_t,32> moduleSha256{};
    std::uint32_t apiVersion=0;
    std::uint64_t encoderGeneration=0;
};
enum class SenderStatus { Off, WaitingPeer, Negotiating, MutualConfirmed,
    ProbeObserved, SourceUnverified, SourceChanged, InvalidRequest,
    ControlRejected, MalformedAccessUnit, PendingSubmission, NothingToSend,
    SubmissionRejected, TokenMismatch, StaleSubmission, OutOfOrder, LimitExceeded };
using Notify = void(*)(SenderStatus,void*);
struct PreparedAccessUnit {
    Bytes output{}; // ORIGINAL borrowed span on every refusal/passthrough.
    std::uint64_t ticket=0; // Nonzero only for one owned pending augmented AU.
    bool appended=false;
    SenderStatus status=SenderStatus::Off;
};

// Single caller-owned encoder worker; no sockets, hooks, render calls, timers or
// background work. No masks/coverage permission is implemented by this prototype.
class SenderAdapter {
public:
    explicit SenderAdapter(Notify notify=nullptr,void* context=nullptr);
    SenderStatus request(const LocalConfig&,const VerifiedChannel&,const VerifiedSource&);
    void stop(); // Revoke future writes, retain an outstanding borrowed lease.
    Status receiveControl(Bytes,const VerifiedChannel&);
    PreparedAccessUnit prepareDiscovery(Bytes,const VerifiedSource&);
    PreparedAccessUnit prepareProbe(Bytes,const VerifiedSource&,std::uint64_t sequence,const Nonce& freshToken);
    // Call only after the actual AU consumer has copied/finished borrowing output.
    // accepted means actual submission, NOT just successful construction.
    Status complete(std::uint64_t ticket,bool accepted,const VerifiedChannel&);
    Status receiveEcho(Bytes,const VerifiedChannel&);
    bool requested() const { return requested_; }
    bool mutualConfirmed() const { return requested_&&handshake_.confirmed(); }
    bool probeObserved() const { return requested_&&probeSent_&&probe_.observed(); }
    bool coveragePermitted() const { return false; } // Same-frame/eye/UV proof absent.
    SenderStatus status() const { return status_; }
    const Handshake& handshake() const { return handshake_; }
private:
    bool admissible(const VerifiedSource&) const;
    bool sameSource(const VerifiedSource&) const;
    PreparedAccessUnit unchanged(Bytes,SenderStatus);
    PreparedAccessUnit append(Bytes,const VerifiedSource&,Bytes);
    void publish(SenderStatus);
    Handshake handshake_{};
    ProbeTracker probe_{},pendingProbe_{};
    VerifiedSource source_{};
    Notify notify_=nullptr;void* context_=nullptr;
    SenderStatus status_=SenderStatus::Off;
    std::uint32_t events_=0;
    std::uint64_t nextTicket_=0,pendingTicket_=0,epoch_=0,pendingEpoch_=0;
    bool requested_=false,pendingIsProbe_=false,probeSent_=false;
    Message pendingMessage_{};
    std::vector<std::uint8_t> owned_{};
};
} // namespace stream_bridge::sender
