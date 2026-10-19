#pragma once
#include "stream-bridge-sender-provider-core.h"
#include "bridge/stream_bridge_control.h"
namespace stream_bridge::sender {
enum class RuntimeState {Off,WaitingEncoder,AmbiguousEncoder,Preparing,WaitingPeer,Negotiating,Confirmed,ProbeObserved,Failed};
enum class RuntimeFailure {
    None,PrepareControl,LocalConfig,RequestDiscovery,TransportLost,
    TransportAuthenticationRefused,TransportDisabled,ReceiveControl,ReceiveEcho,
    ProbeSequenceExhausted,ProbeRandom,RequestProbe
};
struct RuntimeFailureSnapshot {
    RuntimeFailure reason=RuntimeFailure::None;
    ControlStatus control=ControlStatus::Disabled;
    Status protocol=Status::Ok;bool hasProtocolStatus=false;
    EncoderState encoder{};std::uint32_t liveOutputLeases=0;
};
// Actual provider worker owner. No socket/file work occurs in public NVENC or
// DirectMode callbacks. A codec/source change revokes the old channel first.
class SenderRuntime {
public:
    using Clock=std::uint64_t(*)(void*);
    explicit SenderRuntime(NvencBoundary& boundary,Clock clock=nullptr,void* context=nullptr)
        :boundary_(boundary),clock_(clock),clockContext_(context){}
    void configure(const ControlRequest&); // Cold owner, before starting worker.
    void tick(); // Existing provider worker; nonblocking, no render callbacks.
    void stop();
    RuntimeState state()const{return state_;}
    EncoderState encoder()const{return encoder_;}
    // Last failure is retained across source re-admission, until configure/stop.
    // Cold metadata only: no packet contents, keys, sockets or render-thread I/O.
    RuntimeFailureSnapshot lastFailure()const{return failure_;}
    ControlRuntime& control(){return control_;}
private:
    NvencBoundary& boundary_;ControlRuntime control_{};ControlRequest request_{};
    EncoderState encoder_{};RuntimeState state_=RuntimeState::Off;
    RuntimeFailureSnapshot failure_{};
    void fail(RuntimeFailure,Status=Status::Ok,bool hasProtocolStatus=false);
    bool attempted_=false,probeRequested_=false,discoveryPending_=false;std::uint64_t nextProbe_=0;
    Clock clock_=nullptr;void* clockContext_=nullptr;
    std::uint64_t retryAt_=0;std::uint32_t retryDelay_=250;bool retryTransport_=false;
    std::uint64_t now()const;
    void scheduleTransportRetry();
};
const char* runtimeStateName(RuntimeState)noexcept;
const char* runtimeFailureName(RuntimeFailure)noexcept;
} // namespace stream_bridge::sender
