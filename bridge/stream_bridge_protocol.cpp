#include "stream_bridge_protocol.h"
#include <algorithm>

namespace stream_bridge {
namespace {
bool nonzero(const Nonce& n) { return std::any_of(n.begin(),n.end(),[](std::uint8_t x){return x!=0;}); }
bool roleOk(Role r) { return r==Role::Producer||r==Role::Receiver; }
bool kindOk(Kind k) { return k==Kind::Hello||k==Kind::Ack||k==Kind::Commit; }
void put(std::uint8_t* p,std::uint64_t v,unsigned n) { for(unsigned i=0;i<n;++i) p[i]=static_cast<std::uint8_t>(v>>(i*8)); }
std::uint64_t get(const std::uint8_t* p,unsigned n) { std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(i*8);return v; }
Status valid(const Message& m) {
    if(m.major!=ProtocolMajor||m.minor!=ProtocolMinor)return Status::UnsupportedVersion;
    if(!kindOk(m.kind)||!roleOk(m.role)||!m.buildId||!nonzero(m.senderNonce))return Status::Malformed;
    if((m.required&RequiredCapabilities)!=RequiredCapabilities||(m.required&~KnownCapabilities)
        ||(m.required&~m.offered))return Status::IncompatibleCapabilities;
    if(m.kind==Kind::Hello) {
        if(nonzero(m.peerNonce)||m.selected)return Status::Malformed;
    } else if(!nonzero(m.peerNonce)||m.peerNonce==m.senderNonce||!m.selected
        ||(m.selected&~KnownCapabilities)||(m.selected&~m.offered)
        ||(m.selected&m.required)!=m.required)return Status::Malformed;
    return Status::Ok;
}
bool probeValid(const ProbeMessage& p) {
    return (p.kind==ProbeKind::Probe||p.kind==ProbeKind::Echo)&&nonzero(p.producerNonce)
        &&nonzero(p.receiverNonce)&&p.producerNonce!=p.receiverNonce&&p.sequence&&nonzero(p.freshToken);
}
}
bool operator==(const Message& a,const Message& b) {
    return a.kind==b.kind&&a.role==b.role&&a.major==b.major&&a.minor==b.minor&&a.buildId==b.buildId
        &&a.offered==b.offered&&a.senderNonce==b.senderNonce&&a.peerNonce==b.peerNonce
        &&a.selected==b.selected&&a.required==b.required;
}
bool operator!=(const Message& a,const Message& b) { return !(a==b); }
Status encode(const Message& m,std::array<std::uint8_t,MessageBytes>& output) {
    const Status s=valid(m);if(s!=Status::Ok)return s;
    std::array<std::uint8_t,MessageBytes> b{};
    b[0]='S';b[1]='F';b[2]='B';b[3]='1';put(b.data()+4,m.major,2);put(b.data()+6,m.minor,2);
    b[8]=static_cast<std::uint8_t>(m.kind);b[9]=static_cast<std::uint8_t>(m.role);put(b.data()+12,MessageBytes,4);
    put(b.data()+16,m.buildId,8);put(b.data()+24,m.offered,8);
    std::copy(m.senderNonce.begin(),m.senderNonce.end(),b.begin()+32);
    std::copy(m.peerNonce.begin(),m.peerNonce.end(),b.begin()+48);
    put(b.data()+64,m.selected,8);put(b.data()+72,m.required,8);output=b;return Status::Ok;
}
Status decode(Bytes b,Message& output) {
    if(!b.data)return Status::InvalidArgument;
    if(b.size!=MessageBytes||b.data[0]!='S'||b.data[1]!='F'||b.data[2]!='B'||b.data[3]!='1'
        ||b.data[10]||b.data[11]||get(b.data+12,4)!=MessageBytes)return Status::Malformed;
    Message m;m.major=static_cast<std::uint16_t>(get(b.data+4,2));m.minor=static_cast<std::uint16_t>(get(b.data+6,2));
    m.kind=static_cast<Kind>(b.data[8]);m.role=static_cast<Role>(b.data[9]);
    m.buildId=get(b.data+16,8);m.offered=get(b.data+24,8);
    std::copy_n(b.data+32,16,m.senderNonce.begin());std::copy_n(b.data+48,16,m.peerNonce.begin());
    m.selected=get(b.data+64,8);m.required=get(b.data+72,8);
    const Status s=valid(m);if(s==Status::Ok)output=m;return s;
}
Status Handshake::begin(const LocalConfig& c,const VerifiedChannel& ch) {
    if(!ch.authenticated||!nonzero(ch.binding))return Status::Unauthenticated;
    if(!roleOk(c.role)||!c.buildId||!nonzero(c.nonce)||c.nonce==previousLocal_
        ||(configured_&&c.nonce==local_.nonce)||(c.offered&~KnownCapabilities))return Status::InvalidArgument;
    Message test;test.role=c.role;test.buildId=c.buildId;test.senderNonce=c.nonce;test.offered=c.offered;test.required=c.required;
    const Status s=valid(test);if(s!=Status::Ok)return s;
    reset();local_=c;channel_=ch;configured_=true;return Status::Ok;
}
void Handshake::reset() {
    if(configured_)previousLocal_=local_.nonce;
    if(peerKnown_)previousPeer_=peer_.senderNonce;
    local_=LocalConfig{};channel_=VerifiedChannel{};peer_=Message{};
    configured_=peerKnown_=helloSent_=ackSent_=ackReceived_=commitSent_=commitReceived_=false;
}
Status Handshake::channel(const VerifiedChannel& ch) const {
    if(!ch.authenticated||!nonzero(ch.binding))return Status::Unauthenticated;
    if(!configured_||ch.binding!=channel_.binding)return Status::ChannelMismatch;
    return Status::Ok;
}
bool Handshake::channelMatches(const VerifiedChannel& ch) const { return channel(ch)==Status::Ok; }
Message Handshake::make(Kind kind) const {
    Message m;m.kind=kind;m.role=local_.role;m.buildId=local_.buildId;m.offered=local_.offered;
    m.required=local_.required;m.senderNonce=local_.nonce;
    if(kind!=Kind::Hello){m.peerNonce=peer_.senderNonce;m.selected=local_.offered&peer_.offered;}
    return m;
}
Status Handshake::next(Message& output) const {
    if(!configured_)return Status::NoMessage;
    if(!helloSent_)output=make(Kind::Hello);
    else if(peerKnown_&&!ackSent_)output=make(Kind::Ack);
    else if(ackSent_&&ackReceived_&&!commitSent_)output=make(Kind::Commit);
    else return Status::NoMessage;
    return Status::Ok;
}
Status Handshake::markSent(const Message& m,const VerifiedChannel& ch) {
    const Status c=channel(ch);if(c!=Status::Ok)return c;
    if(!kindOk(m.kind)||m!=make(m.kind))return Status::SessionMismatch;
    if((m.kind==Kind::Hello&&helloSent_)||(m.kind==Kind::Ack&&ackSent_)||(m.kind==Kind::Commit&&commitSent_))return Status::Duplicate;
    Message expected;if(next(expected)!=Status::Ok||expected!=m)return Status::OutOfOrder;
    if(m.kind==Kind::Hello)helloSent_=true;else if(m.kind==Kind::Ack)ackSent_=true;else commitSent_=true;
    return Status::Ok;
}
Status Handshake::receive(Bytes bytes,const VerifiedChannel& ch) {
    const Status c=channel(ch);if(c!=Status::Ok)return c;
    Message m;const Status decoded=decode(bytes,m);if(decoded!=Status::Ok)return decoded;
    if(m.role==local_.role)return Status::RoleMismatch;
    if(m.senderNonce==local_.nonce)return Status::SessionMismatch;
    if(m.kind==Kind::Hello){
        if(m.senderNonce==previousPeer_)return Status::SessionMismatch;
        if(peerKnown_)return m==peer_?Status::Duplicate:Status::PeerChanged;
        const auto common=local_.offered&m.offered, need=local_.required|m.required;
        if((common&need)!=need)return Status::IncompatibleCapabilities;
        peer_=m;peerKnown_=true;return Status::Ok;
    }
    if(!peerKnown_||!helloSent_)return Status::OutOfOrder;
    if(m.senderNonce!=peer_.senderNonce||m.peerNonce!=local_.nonce)return Status::SessionMismatch;
    if(m.buildId!=peer_.buildId||m.offered!=peer_.offered||m.required!=peer_.required)return Status::PeerChanged;
    if(m.selected!=(local_.offered&peer_.offered))return Status::IncompatibleCapabilities;
    if(m.kind==Kind::Ack){if(ackReceived_)return Status::Duplicate;ackReceived_=true;return Status::Ok;}
    if(!ackSent_||!ackReceived_)return Status::OutOfOrder;
    if(commitReceived_)return Status::Duplicate;
    commitReceived_=true;return Status::Ok;
}
bool Handshake::confirmed() const { return configured_&&peerKnown_&&helloSent_&&ackSent_&&ackReceived_&&commitSent_&&commitReceived_; }
std::uint64_t Handshake::negotiatedCapabilities() const { return confirmed()?local_.offered&peer_.offered:0; }
Status encodeProbe(const ProbeMessage& p,std::array<std::uint8_t,ProbeBytes>& output) {
    if(!probeValid(p))return Status::InvalidArgument;
    std::array<std::uint8_t,ProbeBytes>b{};b[0]='S';b[1]='F';b[2]='P';b[3]='1';put(b.data()+4,ProtocolMajor,2);put(b.data()+6,ProtocolMinor,2);
    b[8]=static_cast<std::uint8_t>(p.kind);put(b.data()+12,ProbeBytes,4);
    std::copy(p.producerNonce.begin(),p.producerNonce.end(),b.begin()+16);std::copy(p.receiverNonce.begin(),p.receiverNonce.end(),b.begin()+32);
    put(b.data()+48,p.sequence,8);std::copy(p.freshToken.begin(),p.freshToken.end(),b.begin()+56);output=b;return Status::Ok;
}
Status decodeProbe(Bytes b,ProbeMessage& output) {
    if(!b.data)return Status::InvalidArgument;
    if(b.size!=ProbeBytes||b.data[0]!='S'||b.data[1]!='F'||b.data[2]!='P'||b.data[3]!='1'
        ||b.data[9]||b.data[10]||b.data[11]||get(b.data+12,4)!=ProbeBytes)return Status::Malformed;
    for(unsigned i=72;i<80;++i)if(b.data[i])return Status::Malformed;
    if(get(b.data+4,2)!=ProtocolMajor||get(b.data+6,2)!=ProtocolMinor)return Status::UnsupportedVersion;
    ProbeMessage p;p.kind=static_cast<ProbeKind>(b.data[8]);std::copy_n(b.data+16,16,p.producerNonce.begin());
    std::copy_n(b.data+32,16,p.receiverNonce.begin());p.sequence=get(b.data+48,8);std::copy_n(b.data+56,16,p.freshToken.begin());
    if(!probeValid(p))return Status::Malformed;
    output=p;return Status::Ok;
}
Status validateProbe(const Handshake& h,const ProbeMessage& p,const VerifiedChannel& ch) {
    if(!h.channelMatches(ch))return ch.authenticated?Status::ChannelMismatch:Status::Unauthenticated;
    if(!h.confirmed())return Status::OutOfOrder;
    if(!probeValid(p))return Status::Malformed;
    const Nonce producer=h.localRole()==Role::Producer?h.localNonce():h.peerNonce();
    const Nonce receiver=h.localRole()==Role::Receiver?h.localNonce():h.peerNonce();
    return p.producerNonce==producer&&p.receiverNonce==receiver?Status::Ok:Status::SessionMismatch;
}
Status makeProbeEcho(const Handshake& h,const ProbeMessage& p,const VerifiedChannel& ch,ProbeMessage& output) {
    const Status s=validateProbe(h,p,ch);if(s!=Status::Ok)return s;
    if(h.localRole()!=Role::Receiver||p.kind!=ProbeKind::Probe)return Status::RoleMismatch;
    ProbeMessage echo=p;echo.kind=ProbeKind::Echo;output=echo;return Status::Ok;
}
Status ProbeTracker::begin(const Handshake& h,std::uint64_t sequence,const Nonce& token,ProbeMessage& output) {
    if(!h.confirmed())return Status::OutOfOrder;
    if(h.localRole()!=Role::Producer)return Status::RoleMismatch;
    if(!sequence||sequence<=lastSequence_||!nonzero(token)||token==lastToken_)return Status::InvalidArgument;
    ProbeMessage p;p.producerNonce=h.localNonce();p.receiverNonce=h.peerNonce();p.sequence=sequence;p.freshToken=token;
    pending_=p;lastSequence_=sequence;lastToken_=token;pendingValid_=true;observed_=false;output=p;return Status::Ok;
}
Status ProbeTracker::acceptEcho(const Handshake& h,Bytes b,const VerifiedChannel& ch) {
    ProbeMessage p;const Status decoded=decodeProbe(b,p);if(decoded!=Status::Ok)return decoded;
    const Status s=validateProbe(h,p,ch);if(s!=Status::Ok)return s;
    if(h.localRole()!=Role::Producer||p.kind!=ProbeKind::Echo)return Status::RoleMismatch;
    if(!pendingValid_||p.sequence!=pending_.sequence||p.freshToken!=pending_.freshToken
        ||p.producerNonce!=pending_.producerNonce||p.receiverNonce!=pending_.receiverNonce)return Status::SessionMismatch;
    if(observed_)return Status::Duplicate;
    observed_=true;return Status::Ok;
}
void ProbeTracker::reset(){pending_=ProbeMessage{};lastSequence_=0;lastToken_={};pendingValid_=observed_=false;}
} // namespace stream_bridge
