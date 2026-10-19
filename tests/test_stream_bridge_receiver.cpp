#include "../receiver/stream_bridge_receiver.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sb = stream_bridge;
namespace rx = stream_bridge_receiver;
namespace {
unsigned checks = 0;
void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
sb::Nonce nonce(std::uint8_t value) { sb::Nonce n{}; n[0] = value; return n; }
sb::Bytes bytes(const std::vector<std::uint8_t>& value) { return {value.data(), value.size()}; }
template<std::size_t N> sb::Bytes bytes(const std::array<std::uint8_t, N>& value) { return {value.data(), N}; }
const std::vector<std::uint8_t> ordinary{0,0,0,1,0x26,0x01,0x80};

std::vector<std::uint8_t> owned(sb::Bytes payload) {
    std::vector<std::uint8_t> result;
    require(sb::appendOwned(bytes(ordinary), payload, result) == sb::SeiStatus::Ok, "real SEI writer builds AU");
    return result;
}
std::array<std::uint8_t, sb::MessageBytes> encode(const sb::Message& message) {
    std::array<std::uint8_t, sb::MessageBytes> result{};
    require(sb::encode(message, result) == sb::Status::Ok, "real control encoder");
    return result;
}
std::array<std::uint8_t, sb::ProbeBytes> encode(const sb::ProbeMessage& message) {
    std::array<std::uint8_t, sb::ProbeBytes> result{};
    require(sb::encodeProbe(message, result) == sb::Status::Ok, "real probe encoder");
    return result;
}

struct Rig {
    rx::Receiver receiver;
    sb::Handshake producer;
    rx::Authority authority{nonce(9), 1, {true, nonce(10)}};
    sb::LocalConfig local{sb::Role::Receiver, 709, nonce(2)};
    sb::LocalConfig remote{sb::Role::Producer, 710, nonce(1)};
    rx::NativeFrame native{31, 5};
    rx::Surface surface{7, 9, 1};
    Rig() {
        require(receiver.begin(authority, local), "receiver begins with caller authority");
        require(producer.begin(remote, authority.channel) == sb::Status::Ok, "producer begins");
    }
    rx::AccessUnitContext context() const { return {authority, native, true, true}; }
    rx::AccessUnitResult observe(const std::vector<std::uint8_t>& au, rx::AccessUnitContext ctx) {
        const auto copy = au;
        const auto result = receiver.observeAccessUnit(bytes(au), ctx);
        require(result.original.data == au.data() && result.original.size == au.size(), "exact original AU view retained");
        require(copy == au, "all original bytes unchanged");
        return result;
    }
    rx::AccessUnitResult observe(const std::vector<std::uint8_t>& au) { return observe(au, context()); }
    void exchange(bool sendFinalCommit = true) {
        for (unsigned phase = 0; phase < 3; ++phase) {
            sb::Message p, r;
            require(producer.next(p) == sb::Status::Ok, "producer has next exchange action");
            const auto wire = encode(p);
            const auto au = owned(bytes(wire));
            require(producer.markSent(p, authority.channel) == sb::Status::Ok, "producer records actual handoff");
            require(observe(au).result == rx::Result::Handshake, "handshake observed in actual coded AU");
            require(receiver.nextOutbound(r) == sb::Status::Ok, "receiver has reverse action");
            const auto reply = encode(r);
            if (phase != 2 || sendFinalCommit) {
                require(producer.receive(bytes(reply), authority.channel) == sb::Status::Ok, "producer receives reverse action");
                require(receiver.markOutboundSent(r, authority) == sb::Status::Ok, "reverse action marked only after handoff");
            }
        }
    }
    sb::ProbeMessage probe(std::uint64_t sequence = 17, std::uint8_t token = 3) const {
        return {sb::ProbeKind::Probe, remote.nonce, local.nonce, sequence, nonce(token)};
    }
    rx::Result submit(const sb::ProbeMessage& message) {
        const auto wire = encode(message);
        return observe(owned(bytes(wire))).result;
    }
    rx::DecodedContext decoded() const { return {authority, native, surface, true}; }
    rx::SelectedContext selected() const { return {authority, native, surface, true}; }
};

void ordinaryAndAuthority() {
    Rig rig;
    for (unsigned i = 0; i < 20; ++i)
        require(rig.observe(ordinary).result == rx::Result::Ordinary, "unmodified Steam Link is ordinary");
    rx::Notice notice;
    require(!rig.receiver.pollNotice(notice), "ordinary frames do not cause automatic toasts");
    rig.receiver.requestStatus();
    require(rig.receiver.pollNotice(notice) && notice.code == rx::NoticeCode::SenderMissing &&
        notice.session == rig.authority.session && notice.streamGeneration == 1, "explicit missing-sender notice bound to session");
    for (unsigned i = 0; i < 20; ++i) rig.receiver.requestStatus();
    require(!rig.receiver.pollNotice(notice), "missing sender reported once per session");
    auto context = rig.context(); context.sourceBoundaryVerified = false;
    require(rig.observe(ordinary, context).result == rx::Result::UnverifiedBoundary, "unproved provider rejected before parse");
    context = rig.context(); context.completeAnnexBAccessUnit = false;
    require(rig.observe(ordinary, context).result == rx::Result::UnverifiedBoundary, "incomplete AU rejected");
    context = rig.context(); context.native.generation = 0;
    require(rig.observe(ordinary, context).result == rx::Result::UnverifiedBoundary, "native generation not inferred");
    context = rig.context(); context.authority.session = nonce(11);
    require(rig.observe(ordinary, context).result == rx::Result::WrongSession, "another session rejected");
    context = rig.context(); context.authority.channel.authenticated = false;
    require(rig.observe(ordinary, context).result == rx::Result::WrongSession, "unauthenticated channel rejected");
    context = rig.context(); context.authority.channel.binding = nonce(12);
    require(rig.observe(ordinary, context).result == rx::Result::WrongSession, "another verified route rejected");
    context = {}; // Even a malformed untrusted pointer must not be inspected.
    const sb::Bytes invalid{reinterpret_cast<const std::uint8_t*>(1), std::numeric_limits<std::size_t>::max()};
    const auto result = rig.receiver.observeAccessUnit(invalid, context);
    require(result.original.data == invalid.data && result.original.size == invalid.size &&
        result.result == rx::Result::WrongSession, "untrusted view returned without dereference");
    require(!rig.receiver.snapshot().peerConfirmed && !rig.receiver.snapshot().metadataObserved, "ordinary bytes have no bridge authority");
}

void actualMutualHandshake() {
    Rig rig;
    require(rig.submit(rig.probe()) == rx::Result::NotConfirmed, "probe before handshake never recognized");
    rig.exchange(false);
    require(!rig.receiver.snapshot().peerConfirmed && !rig.producer.confirmed(), "unsent own COMMIT cannot confirm either endpoint");
    require(rig.submit(rig.probe()) == rx::Result::NotConfirmed, "uncommitted handshake cannot bind AU metadata");
    sb::Message final;
    require(rig.receiver.nextOutbound(final) == sb::Status::Ok && final.kind == sb::Kind::Commit, "unsent COMMIT remains next");
    require(rig.receiver.markOutboundSent(final, rig.authority) == sb::Status::Ok, "final handoff acknowledged");
    require(rig.receiver.snapshot().peerConfirmed, "both exact commitments establish peer recognition");
    require(!rig.receiver.snapshot().metadataObserved && !rig.receiver.snapshot().metadataSelected,
        "confirmed HELLO/ACK/COMMIT provides no selected-picture proof");
    require(!rig.receiver.selectedMetadata(rig.selected()), "handshake alone has no selected metadata");
    rig.receiver.requestStatus(); rx::Notice notice;
    require(rig.receiver.pollNotice(notice) && notice.code == rx::NoticeCode::PeerConfirmed, "recognition status is not MR readiness");
}

void metadataAndExactSelection() {
    Rig rig; rig.exchange();
    const auto probe = rig.probe();
    require(rig.submit(probe) == rx::Result::MetadataObserved, "tiny probe tied to native coded AU");
    require(rig.submit(probe) == rx::Result::Duplicate, "identical reobservation does not duplicate binding");
    require(rig.receiver.snapshot().probeCount == 1 && rig.receiver.snapshot().metadataObserved &&
        !rig.receiver.snapshot().metadataSelected, "observed is distinct from selected");
    require(!rig.receiver.selectedMetadata(rig.selected()), "no selection before actual decoded association");
    auto decoded = rig.decoded(); decoded.decodedAssociationVerified = false;
    require(rig.receiver.observeDecoded(decoded) == rx::Result::InvalidEvidence, "decoded proof is explicit");
    decoded = rig.decoded(); decoded.surface.generation = 0;
    require(rig.receiver.observeDecoded(decoded) == rx::Result::InvalidEvidence, "unknown content generation rejected");
    require(rig.receiver.observeDecoded(rig.decoded()) == rx::Result::MetadataObserved, "actual decoded tuple recorded");
    auto selected = rig.selected(); selected.selectedAssociationVerified = false;
    require(!rig.receiver.selectedMetadata(selected), "actual selection authority required");
    selected = rig.selected(); selected.native.key++;
    require(!rig.receiver.selectedMetadata(selected), "native key mismatch cannot use latest probe");
    selected = rig.selected(); selected.native.generation++;
    require(!rig.receiver.selectedMetadata(selected), "native key epoch cannot be guessed");
    selected = rig.selected(); selected.surface.namespaceId++;
    require(!rig.receiver.selectedMetadata(selected), "foreign resource namespace rejected");
    selected = rig.selected(); selected.surface.handle++;
    require(!rig.receiver.selectedMetadata(selected), "another surface rejected");
    selected = rig.selected(); selected.surface.generation++;
    require(!rig.receiver.selectedMetadata(selected), "surface allocation alone is not content identity");
    selected = rig.selected(); selected.authority.streamGeneration++;
    require(!rig.receiver.selectedMetadata(selected), "stale stream cannot select new metadata");
    for (unsigned i = 0; i < 5; ++i) {
        const auto result = rig.receiver.selectedMetadata(rig.selected());
        require(result && result->sequence == probe.sequence && result->freshToken == probe.freshToken,
            "repeated actual selection retains exact tiny metadata");
    }
    rig.receiver.requestStatus(); rx::Notice notice;
    require(rig.receiver.pollNotice(notice) && notice.code == rx::NoticeCode::MetadataSelected, "selected status explicitly remains metadata only");

    sb::ProbeMessage echo;
    require(rig.receiver.nextEcho(echo) && echo.kind == sb::ProbeKind::Echo && echo.sequence == probe.sequence,
        "echo only follows observed AU");
    auto changed = echo; changed.freshToken = nonce(20);
    require(!rig.receiver.markEchoSent(changed, rig.authority), "different echo cannot acknowledge pending output");
    auto otherAuthority = rig.authority; otherAuthority.channel.binding = nonce(22);
    require(!rig.receiver.markEchoSent(echo, otherAuthority), "echo handoff requires same channel");
    require(rig.receiver.markEchoSent(echo, rig.authority), "actual echo handoff recorded");
    require(!rig.receiver.nextEcho(echo), "sent echo not sent every frame");
}

void malformedAndUnsupported() {
    Rig rig;
    auto hello = rig.remote;
    sb::Message message; message.role = hello.role; message.buildId = hello.buildId;
    message.senderNonce = hello.nonce;
    auto wire = encode(message); wire[4] = 99;
    require(rig.observe(owned(bytes(wire))).result == rx::Result::InvalidOwned, "unsupported bridge version preserves picture");
    rig.receiver.requestStatus(); rx::Notice notice;
    require(rig.receiver.pollNotice(notice) && notice.code == rx::NoticeCode::SenderIncompatible, "incompatible sender identified once");
    rig.receiver.requestStatus(); require(!rig.receiver.pollNotice(notice), "incompatibility notice deduplicated");
    const std::vector<std::uint8_t> malformed{0,0,0,1,0x4e,1,5,0xff,0};
    require(rig.observe(malformed).result == rx::Result::InvalidOwned, "malformed owned parse is transparent");
    const std::array<std::uint8_t, 3> unknown{{4,5,6}};
    require(rig.observe(owned(bytes(unknown))).result == rx::Result::InvalidOwned, "unknown tiny metadata does not invent peer");
    auto twice = owned(bytes(encode(message))); const auto again = twice;
    twice.insert(twice.end(), again.begin(), again.end());
    require(rig.observe(twice).result == rx::Result::InvalidOwned, "ambiguous duplicate owned data rejected");
    auto context = rig.context();
    const sb::Bytes oversized{reinterpret_cast<const std::uint8_t*>(1), sb::MaximumAccessUnitBytes + 1};
    require(rig.receiver.observeAccessUnit(oversized, context).result == rx::Result::InvalidOwned,
        "size bound checked before oversized AU reads");
    require(!rig.receiver.snapshot().peerConfirmed, "bad/foreign input cannot confirm peer");
}

void lossReuseAndReconnect() {
    Rig rig; rig.exchange();
    require(rig.submit(rig.probe()) == rx::Result::MetadataObserved, "probe established before reuse");
    require(rig.receiver.observeDecoded(rig.decoded()) == rx::Result::MetadataObserved, "decode established before reuse");
    require(bool(rig.receiver.selectedMetadata(rig.selected())), "old selected metadata available");
    auto ordinaryDecoded = rig.decoded(); ordinaryDecoded.native.key++;
    ordinaryDecoded.surface.generation++;
    require(rig.receiver.observeDecoded(ordinaryDecoded) == rx::Result::Ordinary, "ordinary content reuse still tracked");
    require(!rig.receiver.selectedMetadata(rig.selected()), "ordinary reuse invalidates former probe surface");
    require(rig.receiver.observeDecoded(rig.decoded()) == rx::Result::StaleEvidence, "late old decode cannot restore retired content");
    require(!rig.receiver.selectedMetadata(rig.selected()), "stale evidence does not revive selection");
    auto conflicting = ordinaryDecoded; conflicting.native.key++;
    require(rig.receiver.observeDecoded(conflicting) == rx::Result::InvalidEvidence, "same surface generation with two frames is a contradiction");
    require(!rig.receiver.snapshot().evidenceHealthy, "contradiction closes metadata joins only");
    require(rig.observe(ordinary).original.data == ordinary.data(), "ordinary decode unchanged after evidence loss");
    rig.receiver.requestStatus(); rx::Notice notice;
    require(rig.receiver.pollNotice(notice) && notice.code == rx::NoticeCode::EvidenceLost, "bounded loss status");
    require(!rig.receiver.selectedMetadata(rig.selected()), "loss cannot use former metadata");

    const auto oldContext = rig.context();
    rig.receiver.end();
    require(!rig.receiver.begin(rig.authority, rig.local), "same session cannot be resumed as reconnect");
    rig.authority.session = nonce(40); rig.authority.streamGeneration = 2;
    rig.local.nonce = nonce(41); rig.remote.nonce = nonce(42); rig.authority.channel.binding = nonce(43);
    require(rig.receiver.begin(rig.authority, rig.local), "fresh session/nonces/generation reconnect");
    require(rig.producer.begin(rig.remote, rig.authority.channel) == sb::Status::Ok, "producer reconnects with fresh nonce");
    require(rig.observe(ordinary, oldContext).result == rx::Result::WrongSession, "old callback cannot join new session");
    require(!rig.receiver.selectedMetadata(rig.selected()) && rig.receiver.snapshot().probeCount == 0,
        "reconnect discards every previous frame association");
    rig.receiver.requestStatus();
    require(rig.receiver.pollNotice(notice) && notice.code == rx::NoticeCode::SenderMissing && notice.streamGeneration == 2,
        "new session may report its own missing sender once");
    rig.exchange();
    require(rig.submit(rig.probe()) == rx::Result::MetadataObserved, "new-session probe is independent of prior sequence");
    require(rig.receiver.observeDecoded(rig.decoded()) == rx::Result::MetadataObserved, "fresh session has own resource generations");
    require(bool(rig.receiver.selectedMetadata(rig.selected())), "only fresh-session proof restores selected metadata");
}

void capacityAndContradiction() {
    Rig rig; rig.exchange();
    for (std::size_t i = 0; i < rx::Receiver::ProbeCapacity; ++i) {
        rig.native.key = static_cast<std::uint16_t>(100 + i);
        require(rig.submit(rig.probe(1000 + i, static_cast<std::uint8_t>(80 + i))) == rx::Result::MetadataObserved,
            "bounded distinct probe accepted");
    }
    require(rig.receiver.snapshot().probeCount == rx::Receiver::ProbeCapacity, "fixed probe capacity reached");
    rig.native.key = 900;
    require(rig.submit(rig.probe(9000, 180)) == rx::Result::CapacityExceeded, "probe overflow refuses instead of evicting");
    require(!rig.receiver.snapshot().evidenceHealthy, "capacity refusal closes selected metadata");
    require(rig.observe(ordinary).original.size == ordinary.size(), "capacity loss leaves game video unchanged");

    Rig repeated; repeated.exchange();
    require(repeated.submit(repeated.probe()) == rx::Result::MetadataObserved, "first fresh probe");
    repeated.native.key++;
    require(repeated.submit(repeated.probe()) == rx::Result::StaleEvidence, "single probe cannot silently bind a second native frame");
    repeated.native.key--;
    require(repeated.observe(ordinary).result == rx::Result::InvalidEvidence,
        "same native identity cannot silently lose its owned probe");
    require(!repeated.receiver.snapshot().evidenceHealthy, "contradictory AU identity closes joins");
}
} // namespace

int main() {
    try {
        ordinaryAndAuthority(); actualMutualHandshake(); metadataAndExactSelection();
        malformedAndUnsupported(); lossReuseAndReconnect(); capacityAndContradiction();
        std::cout << checks << " stream bridge receiver checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
