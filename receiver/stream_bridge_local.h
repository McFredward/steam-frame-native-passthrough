#pragma once
#include "stream_bridge_native_runtime.h"
#include <memory>
namespace stream_bridge_local {
using KernelIdentity=std::array<std::uint64_t,2>;
struct LeaseReference {
    std::uint32_t slot=0;
    std::uint64_t sequence=0,epoch=0;
    KernelIdentity kernel{};
    stream_bridge_native::CaptureObservation capture{};
};
// One source-process writer, read-only cross-process atomic observers. The
// synchronous capture sink is registered before native decoder creation.
class LeaseWriter {
public:
    LeaseWriter();~LeaseWriter();
    LeaseWriter(const LeaseWriter&)=delete;
    bool prepareCold() noexcept;
    bool associateResourceFd(int) noexcept;
    // Serialized by the actual provider's state lock; never calls I/O/locks.
    void captureEvent(const stream_bridge_native::CaptureObservation&) noexcept;
    static void sink(const stream_bridge_native::CaptureObservation&,void*) noexcept;
    bool reference(const stream_bridge_native::CaptureObservation&,LeaseReference&) const noexcept;
    int duplicateReadOnly() const noexcept;
    // Nonterminal connection/source loss. Old references and already mapped
    // readers remain invalid forever; only a fresh reader can bind the new epoch.
    // Atomic only: never writes the capture sink's slots or native inventory.
    bool invalidateCurrentEpoch() noexcept;
    // Terminal teardown/exhaustion. A terminal writer can never be revived.
    void revoke() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
class LeaseReader {
public:
    LeaseReader();~LeaseReader();
    LeaseReader(const LeaseReader&)=delete;
    // Worker/cold only: verifies fixed seals, owner, read-only fd and layout.
    bool mapReadOnly(int) noexcept;
    // Atomic scalar reads only, never file/socket calls or mutex waits.
    bool current(const LeaseReference&) const noexcept;
    void reset() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
enum class Kind:std::uint8_t{Certified=1,Decoded=2};
struct Message {
    Kind kind=Kind::Certified;
    stream_bridge_native::SourceIdentity source{};
    stream_bridge::FrameDescriptor descriptor{};
    stream_bridge::Nonce binding{};
    std::uint32_t nativeTimestampToken=0;
    std::uint64_t submittedContent=0;
    bool peerConfirmed=false;
    LeaseReference lease{};
    // Detached signed frame header + encoded coverage only; NEVER video/VCL.
    // The consumer must authenticate this and compare the descriptor/binding.
    // This transport does not grant image permission or validate that payload.
    std::vector<std::uint8_t> certifiedMetadata;
    // These fds are BORROWED on enqueue and owned on receive. The caller closes
    // received descriptors after mapping/duplicating them; queue copies its own.
    int dmabuf=-1,readOnlyLeases=-1;
};
enum class Endpoint:std::uint8_t{VrlinkProducer,CompositorConsumer};
enum class State:std::uint8_t{Off,Listening,Connecting,Connected,Refused,Lost,Capacity};
struct PeerPin {std::size_t bytes=0;stream_bridge_native::Hash sha256{};};
PeerPin vrlinkPin() noexcept;
PeerPin compositorPin() noexcept;
// Non-render worker only; no background thread, implicit discovery or services.
// A private pre-existing directory owns the socket; foreign paths are preserved.
// Actual SO_PEERCRED UID/PID plus peer executable hash are required on BOTH ends.
class Channel {
public:
    static constexpr std::size_t MaximumPayload=stream_bridge_native::MaxCodedBytes;
    Channel();~Channel();Channel(const Channel&)=delete;
    bool prepare(Endpoint,const std::filesystem::path& socketPath) noexcept;
    // Explicit test/version-policy port. Shipping prepare uses fixed peer pins.
    bool preparePinned(Endpoint,const std::filesystem::path&,const PeerPin&) noexcept;
    bool enqueue(const Message&) noexcept;
    void tick() noexcept;
    bool receive(Message&) noexcept;
    State state() const noexcept;
    void stop() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
