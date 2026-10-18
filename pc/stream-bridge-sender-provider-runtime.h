#pragma once
#include "stream-bridge-sender-provider-core.h"
#include "bridge/stream_bridge_control.h"
namespace stream_bridge::sender {
enum class RuntimeState {Off,WaitingEncoder,AmbiguousEncoder,Preparing,WaitingPeer,Negotiating,Confirmed,ProbeObserved,Failed};
// Actual provider worker owner. No socket/file work occurs in public NVENC or
// DirectMode callbacks. A codec/source change revokes the old channel first.
class SenderRuntime {
public:
    explicit SenderRuntime(NvencBoundary& boundary):boundary_(boundary){}
    void configure(const ControlRequest&); // Cold owner, before starting worker.
    void tick(); // Existing provider worker; nonblocking, no render callbacks.
    void stop();
    RuntimeState state()const{return state_;}
    EncoderState encoder()const{return encoder_;}
    ControlRuntime& control(){return control_;}
private:
    NvencBoundary& boundary_;ControlRuntime control_{};ControlRequest request_{};
    EncoderState encoder_{};RuntimeState state_=RuntimeState::Off;
    bool attempted_=false,probeRequested_=false;std::uint64_t nextProbe_=0;
};
const char* runtimeStateName(RuntimeState)noexcept;
} // namespace stream_bridge::sender
