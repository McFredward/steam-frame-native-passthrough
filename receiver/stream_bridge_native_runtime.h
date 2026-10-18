#pragma once
#include "stream_bridge_native_decode.h"
#include "bridge/stream_bridge_control.h"
#include <memory>

namespace stream_bridge_native {
// These are source and public decoder facts, never renderer permission. In
// particular the native timestamp is not a PC frame ID or a Vulkan image.
struct SourceIdentity {
    stream_bridge::VideoStreamContext stream{};
    int decoderFd=-1;
    std::uint64_t outputPoolGeneration=0;
};
struct CertifiedFrame {
    SourceIdentity source{};
    Observation submitted{};
    const stream_bridge::CertifiedAccessUnit* certificate=nullptr;
    bool peerConfirmed=false;
};
struct DecodedFrame {
    SourceIdentity source{};
    stream_bridge::FrameDescriptor descriptor{};
    Observation submitted{};
    CaptureObservation capture{};
    bool peerConfirmed=false;
};
enum class RuntimeState:std::uint8_t {
    Disabled,WaitingNativeSource,Negotiating,PeerConfirmed,SourceLost,
    SenderUnavailable,PairingRefused,SourceRefused
};
struct RuntimeNotice {
    RuntimeState state=RuntimeState::Disabled;
    SourceIdentity source{};
    stream_bridge::ControlStatus control=stream_bridge::ControlStatus::Disabled;
};
struct RuntimeCallbacks {
    // Synchronous worker calls. Borrowed certificate/bytes are valid only until
    // return. A consumer that retains them must make its own bounded copy.
    // Callbacks must not reenter start/stop/tick on this worker.
    void (*certified)(const CertifiedFrame&,void*) noexcept=nullptr;
    void (*decoded)(const DecodedFrame&,void*) noexcept=nullptr;
    void (*capture)(const SourceIdentity&,const CaptureObservation&,void*) noexcept=nullptr;
    void (*notice)(const RuntimeNotice&,void*) noexcept=nullptr;
    void* context=nullptr;
};
struct RuntimeRequest {
    bool explicitlyEnabled=false;
    std::uint64_t buildId=0;
    std::filesystem::path pairingsDirectory;
    std::array<char,16> producerAddress{};
    std::uint16_t producerPort=0;
    RuntimeCallbacks callbacks{};
};
// All provider ports are used only by the worker, not by a rendering callback.
// The process entry below supplies these exclusively from the pinned provider.
struct SourceProvider {
    bool (*poll)(Observation&,std::uint8_t*,std::size_t,void*) noexcept=nullptr;
    bool (*capture)(CaptureObservation&,void*) noexcept=nullptr;
    SnapshotStats (*stats)(void*) noexcept=nullptr;
    bool (*random)(void*,std::size_t,void*) noexcept=nullptr;
    void* context=nullptr;
};
// Serialized, explicitly ticked worker core, containing the actual authenticated
// control runtime. Tests can supply public-call observations without a headset;
// production never accepts an arbitrary SourceProvider from a network packet.
class ReceiverWorker {
public:
    static constexpr std::size_t PendingCapacity=32;
    ReceiverWorker();~ReceiverWorker();
    ReceiverWorker(const ReceiverWorker&)=delete;
    ReceiverWorker& operator=(const ReceiverWorker&)=delete;
    bool prepare(const RuntimeRequest&,SourceProvider) noexcept;
    void tick() noexcept;
    void stop() noexcept;
    RuntimeNotice status() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
// Cold explicit entry points. Default-off returns before allocation, file,
// socket, hook or thread work. A single worker owns crypto/socket/certificates;
// decoder hooks only produce their already bounded copies/scalars. The shared
// library must stay loaded (NODELETE); unload is never a stop mechanism.
// Call before native decoder creation: late attachment has no lifecycle proof.
bool startPinnedReceiver(const RuntimeRequest&) noexcept;
void stopPinnedReceiver() noexcept;
bool pinnedReceiverRunning() noexcept;
}
