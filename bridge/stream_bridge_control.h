#pragma once
#include "stream_bridge_credentials.h"
#include "../pc/stream-bridge-sender.h"
#include "../receiver/stream_bridge_receiver.h"
#include <memory>
namespace stream_bridge {
enum class ControlStatus{Disabled,WaitingVideo,Listening,Connecting,TransportConnected,
    PairingRefused,AddressRefused,RandomRefused,IoRefused,AuthenticationRefused,Lost,CapacityRefused};
struct ControlRequest{
    bool explicitlyEnabled=false;
    Role role=Role::Producer;std::uint64_t buildId=0;
    std::filesystem::path pairingsDirectory;
    // Numeric IPv4 only; no DNS/discovery or implicit public listener.
    std::array<char,16> address{};std::uint16_t port=0;
    // Required by receiver, from the actual admitted source lifecycle. Never
    // derive it from this socket, a key file or a local receive counter.
    VideoStreamContext video{};
};
struct VerifiedControl{AuthPayload payload{};VerifiedChannel channel{};};
struct SendReceipt{std::uint64_t ticket=0;};
class ControlRuntime{
public:
    ControlRuntime();~ControlRuntime();
    ControlRuntime(const ControlRuntime&)=delete;
    ControlRuntime& operator=(const ControlRuntime&)=delete;
    // Cold worker only. Default-off returns before file/socket/RNG operations.
    // An explicit prepare loads one private pairing and opens a real socket.
    ControlStatus prepare(const ControlRequest&) noexcept;
    void stop() noexcept;
    // Worker only: no wait/sleep; max sixteen send/recv attempts per tick.
    void tick() noexcept;
    ControlStatus status() const noexcept;
    CredentialStatus credentialStatus() const noexcept;
    std::uint16_t localPort() const noexcept;
    // Configuration only, for receiver.beginAuthenticated before video arrives.
    // This never produces a VerifiedChannel or says the peer is authenticated.
    bool configuredLocal(LocalConfig&) const noexcept;
    bool localConfig(LocalConfig&,LocalSendAuthority&) const noexcept;
    // Direct existing adapter ports. Encoder callback only seals under the
    // try-lock; it never waits behind cold preparation or worker socket I/O.
    // Contention refuses an append and leaves original video unchanged.
    sender::PayloadSealer senderSealer() noexcept;
    stream_bridge_receiver::PayloadVerifier receiverVerifier() noexcept;
    void* verifierContext() noexcept;
    // Reverse receiver->producer control only; producer messages stay in the
    // existing ordered video/SEI route, avoiding cross-route sequence races.
    // Queue accepted iff authenticated bootstrap/peer protection exists. The
    // actual handoff receipt is available only after all176 bytes reach send().
    bool queueControl(Bytes,std::uint64_t& ticket) noexcept;
    bool pollSent(SendReceipt&) noexcept;
    bool pollVerified(VerifiedControl&) noexcept;
    bool peerVerified() const noexcept;
private:
    static bool sealVideo(Bytes,std::vector<std::uint8_t>&,void*);
    static bool verifyVideo(Bytes,const stream_bridge_receiver::AccessUnitContext&,
        AuthPayload&,VerifiedChannel&,void*) noexcept;
    struct Impl;std::unique_ptr<Impl> impl_;
};
const char* controlStatusName(ControlStatus) noexcept;
} // namespace stream_bridge
