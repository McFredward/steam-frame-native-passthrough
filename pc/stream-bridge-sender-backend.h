#pragma once
#include "stream-bridge-sender.h"
#include <array>
#include <cstdint>

namespace stream_bridge::sender {
enum class DiscoveryAdmission { Ready, Busy, Refused };
struct EncoderState {
    void* encoder=nullptr;std::uint64_t generation=0;
    bool hevc=false,peerConfirmed=false,probeObserved=false;
    std::uint32_t width=0,height=0;bool encoded=false;
};
// A typed, allocation-free worker port. It carries no source/coverage proof and
// cannot turn an unknown encoder into a supported one. Backend implementations
// retain the actual SDK/native lifetime and exact-AU authority themselves.
class SenderBackend {
public:
    SenderBackend()=default;
    template<class Boundary> static SenderBackend bind(Boundary& boundary) noexcept {
        SenderBackend result;result.context_=&boundary;
        result.states_=[](void* p){return static_cast<Boundary*>(p)->encoderStates();};
        result.leases_=[](void* p){return static_cast<Boundary*>(p)->liveLeases();};
        result.revoke_=[](void* p,void* encoder,std::uint64_t generation){static_cast<Boundary*>(p)->revokeDiscovery(encoder,generation);};
        result.discovery_=[](void* p,void* encoder,const LocalConfig& local,const VerifiedChannel& channel,PayloadSealer sealer){return static_cast<Boundary*>(p)->tryRequestDiscovery(encoder,local,channel,sealer);};
        result.control_=[](void* p,void* encoder,Bytes bytes,const VerifiedChannel& channel){return static_cast<Boundary*>(p)->receiveControl(encoder,bytes,channel);};
        result.echo_=[](void* p,void* encoder,Bytes bytes,const VerifiedChannel& channel){return static_cast<Boundary*>(p)->receiveEcho(encoder,bytes,channel);};
        result.probe_=[](void* p,void* encoder,std::uint64_t generation,std::uint64_t sequence,const Nonce& token){return static_cast<Boundary*>(p)->requestProbe(encoder,generation,sequence,token);};
        return result;
    }
    std::array<EncoderState,4> encoderStates()const{return states_?states_(context_):std::array<EncoderState,4>{};}
    std::uint32_t liveLeases()const{return leases_?leases_(context_):0;}
    void revokeDiscovery(void* encoder,std::uint64_t generation)const{if(revoke_)revoke_(context_,encoder,generation);}
    DiscoveryAdmission tryRequestDiscovery(void* encoder,const LocalConfig& local,const VerifiedChannel& channel,PayloadSealer sealer)const{return discovery_?discovery_(context_,encoder,local,channel,sealer):DiscoveryAdmission::Refused;}
    Status receiveControl(void* encoder,Bytes bytes,const VerifiedChannel& channel)const{return control_?control_(context_,encoder,bytes,channel):Status::OutOfOrder;}
    Status receiveEcho(void* encoder,Bytes bytes,const VerifiedChannel& channel)const{return echo_?echo_(context_,encoder,bytes,channel):Status::OutOfOrder;}
    bool requestProbe(void* encoder,std::uint64_t generation,std::uint64_t sequence,const Nonce& token)const{return probe_&&probe_(context_,encoder,generation,sequence,token);}
private:
    void* context_=nullptr;
    std::array<EncoderState,4>(*states_)(void*)=nullptr;
    std::uint32_t(*leases_)(void*)=nullptr;
    void(*revoke_)(void*,void*,std::uint64_t)=nullptr;
    DiscoveryAdmission(*discovery_)(void*,void*,const LocalConfig&,const VerifiedChannel&,PayloadSealer)=nullptr;
    Status(*control_)(void*,void*,Bytes,const VerifiedChannel&)=nullptr;
    Status(*echo_)(void*,void*,Bytes,const VerifiedChannel&)=nullptr;
    bool(*probe_)(void*,void*,std::uint64_t,std::uint64_t,const Nonce&)=nullptr;
};
} // namespace stream_bridge::sender
