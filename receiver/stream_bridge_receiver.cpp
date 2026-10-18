#include "stream_bridge_receiver.h"

#include <vector>

namespace stream_bridge_receiver {
namespace {
template<class Id> bool nonzero(const Id& id) noexcept {
    for (const auto value : id) if (value) return true;
    return false;
}
bool same(NativeFrame a, NativeFrame b) noexcept {
    return a.key == b.key && a.generation == b.generation;
}
bool valid(Surface value) noexcept {
    return value.namespaceId && value.handle && value.generation;
}
bool handle(Surface a, Surface b) noexcept {
    return a.namespaceId == b.namespaceId && a.handle == b.handle;
}
bool same(Surface a, Surface b) noexcept {
    return handle(a, b) && a.generation == b.generation;
}
bool same(const stream_bridge::ProbeMessage& a, const stream_bridge::ProbeMessage& b) noexcept {
    return a.kind == b.kind && a.producerNonce == b.producerNonce &&
        a.receiverNonce == b.receiverNonce && a.sequence == b.sequence && a.freshToken == b.freshToken;
}
} // namespace

bool Receiver::begin(const Authority& authority, const stream_bridge::LocalConfig& local) noexcept {
    end();
    if (!nonzero(authority.session) || !authority.streamGeneration ||
        (everBegan_ && (authority.session == previousSession_ || authority.streamGeneration <= lastGeneration_)) ||
        local.role != stream_bridge::Role::Receiver)
        return false;
    if (handshake_.begin(local, authority.channel) != stream_bridge::Status::Ok) return false;
    authority_ = authority;
    previousSession_ = authority.session;
    lastGeneration_ = authority.streamGeneration;
    everBegan_ = active_ = healthy_ = true;
    return true;
}

void Receiver::end() noexcept {
    active_ = healthy_ = peerSeen_ = incompatible_ = observed_ = selected_ = false;
    emitted_ = pending_ = 0;
    probes_ = {};
    surfaces_ = {};
    handshake_.reset();
}

bool Receiver::matches(const Authority& authority) const noexcept {
    return active_ && authority.session == authority_.session &&
        authority.streamGeneration == authority_.streamGeneration &&
        authority.channel.authenticated && authority.channel.binding == authority_.channel.binding;
}

void Receiver::evidenceLost() noexcept { healthy_ = selected_ = false; }

stream_bridge::Status Receiver::receiveControl(stream_bridge::Bytes bytes, const Authority& authority) noexcept {
    if (!matches(authority)) return stream_bridge::Status::ChannelMismatch;
    const auto result = handshake_.receive(bytes, authority.channel);
    if (result == stream_bridge::Status::Ok || result == stream_bridge::Status::Duplicate) peerSeen_ = true;
    else if (result == stream_bridge::Status::UnsupportedVersion ||
             result == stream_bridge::Status::IncompatibleCapabilities ||
             result == stream_bridge::Status::RoleMismatch) incompatible_ = true;
    return result;
}

stream_bridge::Status Receiver::nextOutbound(stream_bridge::Message& message) const noexcept {
    if (!active_) return stream_bridge::Status::InvalidArgument;
    return handshake_.next(message);
}

stream_bridge::Status Receiver::markOutboundSent(const stream_bridge::Message& message,
                                                const Authority& authority) noexcept {
    if (!matches(authority)) return stream_bridge::Status::ChannelMismatch;
    return handshake_.markSent(message, authority.channel);
}

Receiver::ProbeEntry* Receiver::find(NativeFrame native) noexcept {
    for (auto& entry : probes_) if (entry.used && same(entry.native, native)) return &entry;
    return nullptr;
}
const Receiver::ProbeEntry* Receiver::find(NativeFrame native) const noexcept {
    for (const auto& entry : probes_) if (entry.used && same(entry.native, native)) return &entry;
    return nullptr;
}

Result Receiver::recordProbe(const stream_bridge::ProbeMessage& probe, NativeFrame native) noexcept {
    for (const auto& entry : probes_) {
        if (!entry.used) continue;
        if (same(entry.native, native)) {
            if (same(entry.message, probe)) return Result::Duplicate;
            evidenceLost();
            return Result::InvalidEvidence;
        }
        // A fresh single-use token cannot silently acquire another native frame.
        if (entry.message.freshToken == probe.freshToken || entry.message.sequence == probe.sequence)
            return Result::StaleEvidence;
    }
    for (auto& entry : probes_) if (!entry.used) {
        stream_bridge::ProbeMessage echo;
        if (stream_bridge::makeProbeEcho(handshake_, probe, authority_.channel, echo) != stream_bridge::Status::Ok)
            return Result::InvalidOwned;
        entry = {true, true, native, probe, echo};
        observed_ = true;
        return Result::MetadataObserved;
    }
    evidenceLost();
    return Result::CapacityExceeded;
}

AccessUnitResult Receiver::observeAccessUnit(stream_bridge::Bytes original,
                                            const AccessUnitContext& context) noexcept {
    AccessUnitResult output{original, Result::Ordinary};
    if (!matches(context.authority)) { output.result = Result::WrongSession; return output; }
    if (!context.sourceBoundaryVerified || !context.completeAnnexBAccessUnit || !context.native.generation) {
        output.result = Result::UnverifiedBoundary; return output;
    }
    try {
        std::vector<std::uint8_t> payload;
        const auto inspected = stream_bridge::inspectOwned(original, payload);
        if (inspected == stream_bridge::SeiStatus::NotFound) {
            if (find(context.native)) { evidenceLost(); output.result = Result::InvalidEvidence; }
            return output;
        }
        if (inspected != stream_bridge::SeiStatus::Ok) { output.result = Result::InvalidOwned; return output; }
        const stream_bridge::Bytes bytes{payload.data(), payload.size()};
        stream_bridge::Message control;
        if (stream_bridge::decode(bytes, control) == stream_bridge::Status::Ok) {
            const auto status = receiveControl(bytes, context.authority);
            output.result = status == stream_bridge::Status::Ok || status == stream_bridge::Status::Duplicate
                ? Result::Handshake : Result::Incompatible;
            return output;
        }
        stream_bridge::ProbeMessage probe;
        if (stream_bridge::decodeProbe(bytes, probe) != stream_bridge::Status::Ok ||
            probe.kind != stream_bridge::ProbeKind::Probe) {
            // An owned but unsupported version is diagnostic, never a video edit.
            if (stream_bridge::decode(bytes, control) == stream_bridge::Status::UnsupportedVersion)
                incompatible_ = true;
            output.result = Result::InvalidOwned; return output;
        }
        if (!handshake_.confirmed()) { output.result = Result::NotConfirmed; return output; }
        if (!healthy_) { output.result = Result::EvidenceLost; return output; }
        if (stream_bridge::validateProbe(handshake_, probe, context.authority.channel) != stream_bridge::Status::Ok) {
            output.result = Result::InvalidOwned; return output;
        }
        output.result = recordProbe(probe, context.native);
    } catch (...) {
        // Allocation/parser failures cannot propagate into or alter native decode.
        evidenceLost();
        output.result = Result::EvidenceLost;
    }
    return output;
}

bool Receiver::nextEcho(stream_bridge::ProbeMessage& message) const noexcept {
    if (!active_ || !healthy_ || !handshake_.confirmed()) return false;
    for (const auto& entry : probes_) if (entry.used && entry.echoPending) {
        message = entry.echo; return true;
    }
    return false;
}

bool Receiver::markEchoSent(const stream_bridge::ProbeMessage& message, const Authority& authority) noexcept {
    if (!matches(authority) || !healthy_ || !handshake_.confirmed()) return false;
    for (auto& entry : probes_) if (entry.used && entry.echoPending && same(entry.echo, message)) {
        entry.echoPending = false; return true;
    }
    return false;
}

Result Receiver::observeDecoded(const DecodedContext& context) noexcept {
    if (!matches(context.authority)) return Result::WrongSession;
    if (!context.decodedAssociationVerified || !context.native.generation || !valid(context.surface))
        return Result::InvalidEvidence;
    if (!healthy_) return Result::EvidenceLost;
    for (auto& entry : surfaces_) if (entry.used && handle(entry.surface, context.surface)) {
        if (context.surface.generation < entry.surface.generation) return Result::StaleEvidence;
        if (context.surface.generation == entry.surface.generation) {
            if (same(entry.native, context.native)) return Result::Duplicate;
            evidenceLost(); return Result::InvalidEvidence;
        }
        entry.native = context.native;
        entry.surface = context.surface;
        return find(context.native) ? Result::MetadataObserved : Result::Ordinary;
    }
    for (auto& entry : surfaces_) if (!entry.used) {
        entry = {true, context.native, context.surface};
        return find(context.native) ? Result::MetadataObserved : Result::Ordinary;
    }
    evidenceLost();
    return Result::CapacityExceeded;
}

std::optional<stream_bridge::ProbeMessage> Receiver::selectedMetadata(const SelectedContext& context) noexcept {
    selected_ = false;
    if (!matches(context.authority) || !healthy_ || !handshake_.confirmed() ||
        !context.selectedAssociationVerified || !context.native.generation || !valid(context.surface))
        return std::nullopt;
    for (const auto& entry : surfaces_)
        if (entry.used && same(entry.surface, context.surface) && same(entry.native, context.native)) {
            if (const auto* probe = find(context.native)) { selected_ = true; return probe->message; }
        }
    return std::nullopt;
}

void Receiver::notice(NoticeCode code) noexcept {
    const auto bit = static_cast<std::uint16_t>(1u << static_cast<unsigned>(code));
    if (!(emitted_ & bit)) { pending_ |= bit; emitted_ |= bit; }
}

void Receiver::requestStatus() noexcept {
    if (!active_) return;
    if (!healthy_) notice(NoticeCode::EvidenceLost);
    else if (selected_) notice(NoticeCode::MetadataSelected);
    else if (observed_) notice(NoticeCode::MetadataObserved);
    else if (handshake_.confirmed()) notice(NoticeCode::PeerConfirmed);
    else if (incompatible_) notice(NoticeCode::SenderIncompatible);
    else if (peerSeen_) notice(NoticeCode::Negotiating);
    else notice(NoticeCode::SenderMissing);
}

bool Receiver::pollNotice(Notice& output) noexcept {
    if (!active_) return false;
    for (unsigned code = 0; code <= static_cast<unsigned>(NoticeCode::EvidenceLost); ++code) {
        const auto bit = static_cast<std::uint16_t>(1u << code);
        if (pending_ & bit) {
            pending_ &= static_cast<std::uint16_t>(~bit);
            output = {static_cast<NoticeCode>(code), authority_.session, authority_.streamGeneration};
            return true;
        }
    }
    return false;
}

Snapshot Receiver::snapshot() const noexcept {
    Snapshot value{active_, handshake_.confirmed(), observed_, selected_, healthy_, 0};
    for (const auto& probe : probes_) if (probe.used) ++value.probeCount;
    return value;
}

} // namespace stream_bridge_receiver
