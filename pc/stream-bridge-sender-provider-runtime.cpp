#include "stream-bridge-sender-provider-runtime.h"
namespace stream_bridge::sender {
void SenderRuntime::configure(const ControlRequest& request){stop();request_=request;state_=request.explicitlyEnabled?RuntimeState::WaitingEncoder:RuntimeState::Off;}
void SenderRuntime::stop(){if(encoder_.encoder)boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();encoder_={};attempted_=probeRequested_=false;request_.explicitlyEnabled=false;state_=RuntimeState::Off;}
void SenderRuntime::tick(){
    if(!request_.explicitlyEnabled)return;
    EncoderState current{};unsigned candidates=0;
    for(const auto& e:boundary_.encoderStates())if(e.encoder&&e.hevc&&e.encoded){current=e;++candidates;}
    if(candidates!=1){if(encoder_.encoder){boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();encoder_={};attempted_=probeRequested_=false;}state_=candidates?RuntimeState::AmbiguousEncoder:RuntimeState::WaitingEncoder;return;}
    if(current.encoder!=encoder_.encoder||current.generation!=encoder_.generation){
        if(encoder_.encoder)boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);
        control_.stop();encoder_=current;attempted_=probeRequested_=false;
    }else encoder_=current;
    if(!attempted_){
        attempted_=true;state_=RuntimeState::Preparing;
        const auto result=control_.prepare(request_);LocalConfig local;LocalSendAuthority authority;
        if(result!=ControlStatus::Listening||!control_.localConfig(local,authority)||!boundary_.requestDiscovery(encoder_.encoder,local,authority.channel,control_.senderSealer())){control_.stop();state_=RuntimeState::Failed;return;}
    }
    if(state_==RuntimeState::Failed)return;
    control_.tick();const auto transport=control_.status();
    if(transport==ControlStatus::Lost||transport==ControlStatus::AuthenticationRefused||transport==ControlStatus::Disabled){boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);state_=RuntimeState::Failed;return;}
    VerifiedControl received;
    for(unsigned i=0;i<4&&control_.pollVerified(received);++i){
        ProbeMessage probe;
        const auto result=decodeProbe({received.payload.data(),received.payload.size()},probe)==Status::Ok
            ?boundary_.receiveEcho(encoder_.encoder,{received.payload.data(),received.payload.size()},received.channel)
            :boundary_.receiveControl(encoder_.encoder,{received.payload.data(),received.payload.size()},received.channel);
        if(result!=Status::Ok&&result!=Status::Duplicate){boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();state_=RuntimeState::Failed;return;}
    }
    for(const auto& value:boundary_.encoderStates())if(value.encoder==encoder_.encoder&&value.generation==encoder_.generation)encoder_=value;
    if(encoder_.peerConfirmed&&!probeRequested_){Nonce token{};
        if(nextProbe_==~std::uint64_t(0)||!bridgeSystemRandom(token.data(),token.size())||!boundary_.requestProbe(encoder_.encoder,encoder_.generation,++nextProbe_,token)){boundary_.revokeDiscovery(encoder_.encoder,encoder_.generation);control_.stop();state_=RuntimeState::Failed;return;}
        probeRequested_=true;
    }
    state_=encoder_.probeObserved?RuntimeState::ProbeObserved:encoder_.peerConfirmed?RuntimeState::Confirmed:control_.peerVerified()?RuntimeState::Negotiating:RuntimeState::WaitingPeer;
}
const char* runtimeStateName(RuntimeState state)noexcept{switch(state){case RuntimeState::Off:return "off";case RuntimeState::WaitingEncoder:return "waiting-supported-encoder";case RuntimeState::AmbiguousEncoder:return "multiple-encoders-refused";case RuntimeState::Preparing:return "preparing";case RuntimeState::WaitingPeer:return "waiting-paired-peer";case RuntimeState::Negotiating:return "authenticating-peer";case RuntimeState::Confirmed:return "compatible-peer-confirmed";case RuntimeState::ProbeObserved:return "coded-au-roundtrip-confirmed-not-coverage";default:return "source-or-control-failed-ordinary-video";}}
} // namespace stream_bridge::sender
