#include "stream-bridge-sender.h"
#include <algorithm>
#include <limits>

namespace stream_bridge::sender {
namespace {
template<class T> bool nonzero(const T& value) {
    return std::any_of(value.begin(),value.end(),[](auto b){return b!=0;});
}
}
SenderAdapter::SenderAdapter(Notify notify,void* context):notify_(notify),context_(context){}
void SenderAdapter::publish(SenderStatus value) {
    if(status_==value)return;
    status_=value;
    if(notify_&&(events_<32||value==SenderStatus::Off)) {
        ++events_;
        try {notify_(value,context_);}catch(...){} // Optional telemetry cannot affect encoding.
    }
}
bool SenderAdapter::admissible(const VerifiedSource& v)const {
    return v.verified&&v.ordinaryStream&&v.completeAccessUnit&&
        v.codec==VerifiedSource::Codec::HevcAnnexB&&nonzero(v.moduleSha256)&&
        v.apiVersion&&v.encoderGeneration;
}
bool SenderAdapter::sameSource(const VerifiedSource& v)const {
    return admissible(v)&&v.moduleSha256==source_.moduleSha256&&
        v.apiVersion==source_.apiVersion&&v.encoderGeneration==source_.encoderGeneration;
}
SenderStatus SenderAdapter::request(const LocalConfig& c,const VerifiedChannel& channel,
                                   const VerifiedSource& source,PayloadSealer sealer) {
    if(pendingTicket_){publish(SenderStatus::PendingSubmission);return status_;}
    stop();events_=0;
    // Advertise only the implemented known contract; per-frame proof is still
    // separate and mutual confirmation alone cannot activate coverage.
    if(c.role!=Role::Producer||(c.offered&~KnownCapabilities)||(c.required&~KnownCapabilities)||
       (c.offered&RequiredCapabilities)!=RequiredCapabilities||
       (c.required&RequiredCapabilities)!=RequiredCapabilities||
       (c.required&c.offered)!=c.required){publish(SenderStatus::InvalidRequest);return status_;}
    if(!admissible(source)){publish(SenderStatus::SourceUnverified);return status_;}
    if(handshake_.begin(c,channel)!=Status::Ok){publish(SenderStatus::ControlRejected);return status_;}
    source_=source;sealer_=sealer;requested_=true;++epoch_;publish(SenderStatus::WaitingPeer);return status_;
}
void SenderAdapter::stop() {
    requested_=false;++epoch_;handshake_.reset();probe_.reset();pendingProbe_.reset();
    probeSent_=false;sealer_={};publish(SenderStatus::Off);
    // A consumer may still borrow owned_: only matching complete() releases it.
}
Status SenderAdapter::receiveControl(Bytes data,const VerifiedChannel& channel) {
    if(!requested_)return Status::OutOfOrder;
    auto result=handshake_.receive(data,channel);
    if(result==Status::Ok||result==Status::Duplicate)
        publish(handshake_.confirmed()?SenderStatus::MutualConfirmed:SenderStatus::Negotiating);
    else publish(SenderStatus::ControlRejected);
    return result;
}
PreparedAccessUnit SenderAdapter::unchanged(Bytes input,SenderStatus status) {
    publish(status);return {input,0,false,status_};
}
PreparedAccessUnit SenderAdapter::append(Bytes input,const VerifiedSource& source,Bytes payload) {
    if(!requested_)return unchanged(input,SenderStatus::Off);
    if(pendingTicket_)return unchanged(input,SenderStatus::PendingSubmission);
    if(!sameSource(source)) {
        stop();return unchanged(input,SenderStatus::SourceChanged);
    }
    if(nextTicket_==std::numeric_limits<std::uint64_t>::max())
        return unchanged(input,SenderStatus::LimitExceeded);
    SeiStatus result;
    try {
        std::vector<std::uint8_t> sealed;
        if(sealer_.seal){
            if(!sealer_.seal(payload,sealed,sealer_.context)||sealed.empty()||sealed.size()>MaximumOwnedPayloadBytes)
                return unchanged(input,SenderStatus::ControlRejected);
            payload={sealed.data(),sealed.size()};
        }
        result=appendOwned(input,payload,owned_);
    }catch(...){result=SeiStatus::LimitExceeded;}
    if(result!=SeiStatus::Ok)
        return unchanged(input,result==SeiStatus::LimitExceeded?SenderStatus::LimitExceeded:
                         SenderStatus::MalformedAccessUnit);
    pendingTicket_=++nextTicket_;pendingEpoch_=epoch_;
    return {{owned_.data(),owned_.size()},pendingTicket_,true,status_};
}
PreparedAccessUnit SenderAdapter::prepareDiscovery(Bytes input,const VerifiedSource& source) {
    if(!requested_)return unchanged(input,SenderStatus::Off);
    if(pendingTicket_)return unchanged(input,SenderStatus::PendingSubmission);
    Message message{};
    if(handshake_.next(message)!=Status::Ok)return unchanged(input,SenderStatus::NothingToSend);
    std::array<std::uint8_t,MessageBytes> payload{};
    if(encode(message,payload)!=Status::Ok)return unchanged(input,SenderStatus::InvalidRequest);
    auto prepared=append(input,source,{payload.data(),payload.size()});
    if(prepared.appended){pendingMessage_=message;pendingIsProbe_=false;}
    return prepared;
}
PreparedAccessUnit SenderAdapter::prepareProbe(Bytes input,const VerifiedSource& source,
                                              std::uint64_t sequence,const Nonce& token) {
    if(!requested_)return unchanged(input,SenderStatus::Off);
    if(pendingTicket_)return unchanged(input,SenderStatus::PendingSubmission);
    auto next=probe_;ProbeMessage message{};
    if(next.begin(handshake_,sequence,token,message)!=Status::Ok)
        return unchanged(input,SenderStatus::OutOfOrder);
    std::array<std::uint8_t,ProbeBytes> payload{};
    if(encodeProbe(message,payload)!=Status::Ok)return unchanged(input,SenderStatus::InvalidRequest);
    auto prepared=append(input,source,{payload.data(),payload.size()});
    if(prepared.appended){pendingProbe_=next;pendingIsProbe_=true;}
    return prepared;
}
Status SenderAdapter::complete(std::uint64_t ticket,bool accepted,const VerifiedChannel& channel) {
    if(!ticket||ticket!=pendingTicket_)return Status::SessionMismatch;
    const bool current=requested_&&pendingEpoch_==epoch_;
    Status result=Status::OutOfOrder;
    if(current&&accepted) {
        if(!handshake_.channelMatches(channel))result=Status::ChannelMismatch;
        else if(pendingIsProbe_){probe_=pendingProbe_;probeSent_=true;result=Status::Ok;}
        else result=handshake_.markSent(pendingMessage_,channel);
    }
    pendingTicket_=0;pendingEpoch_=0;pendingIsProbe_=false;owned_.clear();
    if(!current){publish(SenderStatus::StaleSubmission);return result;}
    if(!accepted){publish(SenderStatus::SubmissionRejected);return Status::Ok;}
    if(result==Status::Ok)
        publish(handshake_.confirmed()?SenderStatus::MutualConfirmed:SenderStatus::Negotiating);
    else publish(SenderStatus::ControlRejected);
    return result;
}
Status SenderAdapter::receiveEcho(Bytes data,const VerifiedChannel& channel) {
    if(!requested_||!probeSent_)return Status::OutOfOrder;
    auto result=probe_.acceptEcho(handshake_,data,channel);
    publish(result==Status::Ok?SenderStatus::ProbeObserved:SenderStatus::TokenMismatch);
    return result;
}
} // namespace stream_bridge::sender
