#include "stream-bridge-sender-provider-runtime.h"
#include <algorithm>
#include <chrono>
#include <limits>
namespace stream_bridge::sender {
std::uint64_t SenderRuntime::now()const{
    return clock_?clock_(clockContext_):static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void SenderRuntime::scheduleTransportRetry(){
    const auto clock=now();retryAt_=clock>std::numeric_limits<std::uint64_t>::max()-retryDelay_
        ?std::numeric_limits<std::uint64_t>::max():clock+retryDelay_;
    retryDelay_=std::min<std::uint32_t>(2000,retryDelay_*2);retryTransport_=true;
}
void SenderRuntime::configure(const ControlRequest& request){stop();request_=request;state_=request.explicitlyEnabled?RuntimeState::WaitingEncoder:RuntimeState::Off;}
void SenderRuntime::stop(){if(encoder_.encoder)boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();encoder_={};attempted_=probeRequested_=discoveryPending_=false;request_.explicitlyEnabled=false;state_=RuntimeState::Off;failure_={};retryTransport_=false;retryAt_=0;retryDelay_=250;}
void SenderRuntime::fail(RuntimeFailure reason,Status protocol,bool hasProtocolStatus){
    failure_={reason,control_.status(),protocol,hasProtocolStatus,encoder_,boundary_.liveLeases()};state_=RuntimeState::Failed;discoveryPending_=false;
}
void SenderRuntime::tick(){
    if(!request_.explicitlyEnabled)return;
    EncoderState current{};unsigned candidates=0;
    for(const auto& e:boundary_.encoderStates())if(e.encoder&&e.hevc&&e.encoded){current=e;++candidates;}
    if(candidates!=1){if(encoder_.encoder){boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();encoder_={};attempted_=probeRequested_=discoveryPending_=false;}state_=candidates?RuntimeState::AmbiguousEncoder:RuntimeState::WaitingEncoder;return;}
    if(current.encoder!=encoder_.encoder||current.generation!=encoder_.generation){
        retryTransport_=false;retryAt_=0;retryDelay_=250;
        if(encoder_.encoder)boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);
        control_.stop();encoder_=current;attempted_=probeRequested_=discoveryPending_=false;
    }else encoder_=current;
    // A normal receiver/source restart can close our auxiliary socket while
    // the native encoder stays unchanged. Revoke immediately, then create a
    // fresh binding and complete handshake instead of latching Failed forever.
    // Authentication/protocol/explicit-stop failures never enter this path.
    if(state_==RuntimeState::Failed&&retryTransport_){
        if(now()<retryAt_)return;
        retryTransport_=false;attempted_=probeRequested_=discoveryPending_=false;
    }
    if(!attempted_){
        attempted_=true;state_=RuntimeState::Preparing;
        const auto result=control_.prepare(request_);
        if(result!=ControlStatus::Listening){fail(RuntimeFailure::PrepareControl);control_.stop();return;}
        discoveryPending_=true;
    }
    if(discoveryPending_){
        LocalConfig local;LocalSendAuthority authority;
        if(!control_.localConfig(local,authority)){fail(RuntimeFailure::LocalConfig);control_.stop();return;}
        const auto admission=boundary_.tryRequestDiscovery(encoder_.encoder,local,authority.channel,control_.senderSealer());
        if(admission==DiscoveryAdmission::Busy){state_=RuntimeState::Preparing;return;}
        if(admission!=DiscoveryAdmission::Ready){fail(RuntimeFailure::RequestDiscovery);control_.stop();return;}
        discoveryPending_=false;
    }
    if(state_==RuntimeState::Failed)return;
    control_.tick();const auto transport=control_.status();
    if(transport==ControlStatus::Lost||transport==ControlStatus::AuthenticationRefused||transport==ControlStatus::Disabled){
        fail(transport==ControlStatus::Lost?RuntimeFailure::TransportLost:transport==ControlStatus::AuthenticationRefused
            ?RuntimeFailure::TransportAuthenticationRefused:RuntimeFailure::TransportDisabled);
        boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);
        if(transport==ControlStatus::Lost){control_.stop();scheduleTransportRetry();}
        return;
    }
    VerifiedControl received;
    for(unsigned i=0;i<4&&control_.pollVerified(received);++i){
        ProbeMessage probe;
        const bool echo=decodeProbe({received.payload.data(),received.payload.size()},probe)==Status::Ok;
        const auto result=echo
            ?boundary_.receiveEcho(encoder_.encoder,{received.payload.data(),received.payload.size()},received.channel)
            :boundary_.receiveControl(encoder_.encoder,{received.payload.data(),received.payload.size()},received.channel);
        if(result!=Status::Ok&&result!=Status::Duplicate){fail(echo?RuntimeFailure::ReceiveEcho:RuntimeFailure::ReceiveControl,result,true);boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();return;}
    }
    for(const auto& value:boundary_.encoderStates())if(value.encoder==encoder_.encoder&&value.generation==encoder_.generation)encoder_=value;
    if(encoder_.peerConfirmed&&!probeRequested_){Nonce token{};
        if(nextProbe_==~std::uint64_t(0)){fail(RuntimeFailure::ProbeSequenceExhausted);boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();return;}
        if(!bridgeSystemRandom(token.data(),token.size())){fail(RuntimeFailure::ProbeRandom);boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();return;}
        if(!boundary_.requestProbe(encoder_.encoder,encoder_.generation,++nextProbe_,token)){fail(RuntimeFailure::RequestProbe);boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();return;}
        probeRequested_=true;
    }
    if(encoder_.peerConfirmed)retryDelay_=250;
    state_=encoder_.probeObserved?RuntimeState::ProbeObserved:encoder_.peerConfirmed?RuntimeState::Confirmed:control_.peerVerified()?RuntimeState::Negotiating:RuntimeState::WaitingPeer;
}
const char* runtimeStateName(RuntimeState state)noexcept{switch(state){case RuntimeState::Off:return "off";case RuntimeState::WaitingEncoder:return "waiting-supported-encoder";case RuntimeState::AmbiguousEncoder:return "multiple-encoders-refused";case RuntimeState::Preparing:return "preparing";case RuntimeState::WaitingPeer:return "waiting-paired-peer";case RuntimeState::Negotiating:return "authenticating-peer";case RuntimeState::Confirmed:return "compatible-peer-confirmed";case RuntimeState::ProbeObserved:return "coded-au-roundtrip-confirmed-not-coverage";default:return "source-or-control-failed-ordinary-video";}}
const char* runtimeFailureName(RuntimeFailure reason)noexcept{switch(reason){
    case RuntimeFailure::None:return "none";case RuntimeFailure::PrepareControl:return "prepare-control";
    case RuntimeFailure::LocalConfig:return "local-control-authority";case RuntimeFailure::RequestDiscovery:return "request-discovery";
    case RuntimeFailure::TransportLost:return "transport-lost";case RuntimeFailure::TransportAuthenticationRefused:return "transport-authentication-refused";
    case RuntimeFailure::TransportDisabled:return "transport-disabled";case RuntimeFailure::ReceiveControl:return "receive-control";
    case RuntimeFailure::ReceiveEcho:return "receive-probe-echo";case RuntimeFailure::ProbeSequenceExhausted:return "probe-sequence-exhausted";
    case RuntimeFailure::ProbeRandom:return "probe-random-source";case RuntimeFailure::RequestProbe:return "request-probe";
    default:return "unknown";
}}
} // namespace stream_bridge::sender
