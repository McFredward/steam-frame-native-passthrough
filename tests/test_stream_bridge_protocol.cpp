#include "../bridge/stream_bridge_protocol.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace stream_bridge;
namespace {
unsigned checks=0;
void require(bool ok,const char* name){++checks;if(!ok)throw std::runtime_error(name);}
Nonce nonce(unsigned seed){Nonce n{};for(unsigned i=0;i<n.size();++i)n[i]=static_cast<std::uint8_t>(seed+i);return n;}
std::vector<std::uint8_t> hex(const std::string& s){std::vector<std::uint8_t>b;std::string digits;for(char c:s)if(c!=' ')digits+=c;for(std::size_t i=0;i<digits.size();i+=2)b.push_back(static_cast<std::uint8_t>(std::stoul(digits.substr(i,2),nullptr,16)));return b;}
Bytes view(const std::vector<std::uint8_t>& b){return {b.data(),b.size()};}
Bytes view(const std::array<std::uint8_t,MessageBytes>& b){return {b.data(),b.size()};}
VerifiedChannel channel(){return {true,nonce(160)};}
LocalConfig config(Role r){LocalConfig c;c.role=r;c.buildId=r==Role::Producer?0x0807060504030201ULL:0x1122334455667788ULL;c.nonce=nonce(r==Role::Producer?1:33);c.offered=r==Role::Producer?31:11;return c;}
std::array<std::uint8_t,MessageBytes> wire(const Message& m){std::array<std::uint8_t,MessageBytes>b{};require(encode(m,b)==Status::Ok,"message encoding");return b;}
Message relay(Handshake& from,Handshake& to,Kind kind){Message m;require(from.next(m)==Status::Ok&&m.kind==kind,"expected outbound kind");auto b=wire(m);require(from.markSent(m,channel())==Status::Ok,"actual sent receipt");require(to.receive(view(b),channel())==Status::Ok,"peer receives message");return m;}
void confirmed(Handshake& p,Handshake& r){require(p.begin(config(Role::Producer),channel())==Status::Ok,"producer begin");require(r.begin(config(Role::Receiver),channel())==Status::Ok,"receiver begin");relay(p,r,Kind::Hello);relay(r,p,Kind::Hello);require(!p.confirmed()&&!r.confirmed(),"hello not confirmation");relay(p,r,Kind::Ack);relay(r,p,Kind::Ack);require(!p.confirmed()&&!r.confirmed(),"ack not confirmation");relay(p,r,Kind::Commit);require(!p.confirmed()&&!r.confirmed(),"own unsent commit not confirmation");relay(r,p,Kind::Commit);require(p.confirmed()&&r.confirmed(),"bilateral commits confirmed");require(p.negotiatedCapabilities()==11&&r.negotiatedCapabilities()==11,"capability intersection");}
void goldens(){
    // Literal offsets/endian values independently read from the published wire table.
    const auto golden=hex("53 46 42 31 01 00 00 00 01 01 00 00 50 00 00 00 "
        "01 02 03 04 05 06 07 08 1f 00 00 00 00 00 00 00 "
        "01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 0b 00 00 00 00 00 00 00");
    require(golden.size()==80,"golden exact length");Handshake h;require(h.begin(config(Role::Producer),channel())==Status::Ok,"golden config");Message m;require(h.next(m)==Status::Ok,"golden hello");auto actual=wire(m);require(std::equal(actual.begin(),actual.end(),golden.begin()),"hand checked hello wire");Message parsed;require(decode(view(golden),parsed)==Status::Ok&&parsed==m,"independent hello decoding");
    const auto ack=hex("53 46 42 31 01 00 00 00 02 02 00 00 50 00 00 00 "
        "88 77 66 55 44 33 22 11 0b 00 00 00 00 00 00 00 "
        "21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f 30 "
        "01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 "
        "0b 00 00 00 00 00 00 00 0b 00 00 00 00 00 00 00");
    require(decode(view(ack),parsed)==Status::Ok&&parsed.kind==Kind::Ack&&parsed.role==Role::Receiver&&parsed.buildId==0x1122334455667788ULL&&parsed.selected==11&&parsed.peerNonce==nonce(1),"hand checked ACK fields");
    auto malformed=golden;const Message unchanged=parsed;
    for(unsigned at:{0u,4u,6u,8u,9u,10u,11u,12u}){auto bad=malformed;bad[at]^=0x40;require(decode(view(bad),parsed)!=Status::Ok&&parsed==unchanged,"invalid header does not change output");}
    for(std::size_t size=0;size<80;++size)require(decode({golden.data(),size},parsed)!=Status::Ok&&parsed==unchanged,"all truncated lengths");
    require(decode({nullptr,80},parsed)==Status::InvalidArgument&&parsed==unchanged,"null input");
    auto bad=m;bad.major=2;auto saved=actual;require(encode(bad,actual)==Status::UnsupportedVersion&&actual==saved,"encode output transactional");
}
void invalidState(){
    Handshake p,r;require(p.begin(config(Role::Producer),channel())==Status::Ok,"invalid-state producer");require(r.begin(config(Role::Receiver),channel())==Status::Ok,"invalid-state receiver");Message rh;require(r.next(rh)==Status::Ok,"remote hello");auto validWire=wire(rh);
    auto wrong=channel();wrong.authenticated=false;require(p.receive(view(validWire),wrong)==Status::Unauthenticated,"unauthenticated hello rejected");wrong=channel();wrong.binding[0]^=1;require(p.receive(view(validWire),wrong)==Status::ChannelMismatch,"other channel rejected");require(p.peerNonce()==Nonce{}&&!p.confirmed(),"rejected hello no state");
    Message same=rh;same.role=Role::Producer;auto b=wire(same);require(p.receive(view(b),channel())==Status::RoleMismatch,"same role rejected");
    same=rh;same.senderNonce=config(Role::Producer).nonce;b=wire(same);require(p.receive(view(b),channel())==Status::SessionMismatch,"same nonce rejected");
    same=rh;same.offered=15;same.required=15;b=wire(same);auto pc=config(Role::Producer);pc.offered=11;Handshake restricted;require(restricted.begin(pc,channel())==Status::Ok,"restricted setup");require(restricted.receive(view(b),channel())==Status::IncompatibleCapabilities&&restricted.peerNonce()==Nonce{},"peer requirements matter");
    same=rh;same.kind=Kind::Ack;same.peerNonce=config(Role::Producer).nonce;same.selected=11;b=wire(same);require(p.receive(view(b),channel())==Status::OutOfOrder,"ACK cannot replace HELLO");
    require(p.receive(view(validWire),channel())==Status::Ok,"valid hello after rejected input");require(p.receive(view(validWire),channel())==Status::Duplicate,"duplicate hello stable");
    auto changed=rh;changed.buildId++;b=wire(changed);require(p.receive(view(b),channel())==Status::PeerChanged,"peer build immutable");changed=rh;changed.senderNonce[0]^=16;b=wire(changed);require(p.receive(view(b),channel())==Status::PeerChanged,"new nonce needs explicit reconnect");
    Message own;require(p.next(own)==Status::Ok,"own HELLO still pending");auto forged=own;forged.kind=Kind::Commit;require(p.markSent(forged,channel())!=Status::Ok&&!p.confirmed(),"cannot mark unplanned COMMIT");
    require(p.markSent(own,channel())==Status::Ok,"mark actual hello");require(p.markSent(own,channel())==Status::Duplicate,"duplicate local send receipt");
    auto premature=rh;premature.kind=Kind::Commit;premature.peerNonce=p.localNonce();premature.selected=11;b=wire(premature);require(p.receive(view(b),channel())==Status::OutOfOrder&&!p.confirmed(),"COMMIT before ACK rejected");
    auto invalid=config(Role::Producer);invalid.nonce={};require(p.begin(invalid,channel())==Status::InvalidArgument&&p.peerNonce()==rh.senderNonce,"invalid begin leaves old state");
    p.reset();require(!p.configured()&&!p.confirmed()&&p.negotiatedCapabilities()==0,"transport loss removes trust");require(p.begin(config(Role::Producer),channel())==Status::InvalidArgument,"immediate local nonce reuse refused");auto fresh=config(Role::Producer);fresh.nonce=nonce(65);require(p.begin(fresh,channel())==Status::Ok,"fresh reconnect");require(p.receive(view(b),channel())==Status::OutOfOrder,"old COMMIT not new handshake");require(p.receive(view(validWire),channel())==Status::SessionMismatch&&p.peerNonce()==Nonce{},"previous peer HELLO cannot preempt new session");
}
void compressedCoverageNegotiation(){
    auto full=config(Role::Producer);full.offered=full.required=KnownCapabilities;
    auto old=config(Role::Receiver);old.offered=old.required=KnownCapabilities&~CapRleLz4;
    Handshake producer,receiver;
    require(producer.begin(full,channel())==Status::Ok&&receiver.begin(old,channel())==Status::Ok,"paired codec and old participant configurations are valid");
    Message hello;require(receiver.next(hello)==Status::Ok,"old participant hello available");
    auto encoded=wire(hello);
    require(producer.receive(view(encoded),channel())==Status::IncompatibleCapabilities&&!producer.confirmed(),"current native deployment refuses an old peer before sending compressed masks");
    // The optional protocol capability can still be negotiated away by callers
    // that explicitly support the older raw/RLE route. Native deployment requires it.
    full.required&=~CapRleLz4;
    Handshake optional,legacy;
    require(optional.begin(full,channel())==Status::Ok&&legacy.begin(old,channel())==Status::Ok,"optional compression configuration accepted");
    for(const auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){relay(optional,legacy,kind);relay(legacy,optional,kind);}
    require(optional.confirmed()&&legacy.confirmed()&&!(optional.negotiatedCapabilities()&CapRleLz4),"optional compression never appears without both peers advertising it");
    Handshake p,r;auto modern=config(Role::Receiver);modern.offered=modern.required=KnownCapabilities;
    full.required=KnownCapabilities;
    require(p.begin(full,channel())==Status::Ok&&r.begin(modern,channel())==Status::Ok,"current paired codec configurations accepted");
    for(const auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){relay(p,r,kind);relay(r,p,kind);}
    require(p.confirmed()&&r.confirmed()&&(p.negotiatedCapabilities()&CapRleLz4),"compressed coverage needs a complete mutually confirmed capability exchange");
}
void probe(){
    Handshake p,r;ProbeTracker t;ProbeMessage request;require(t.begin(p,7,nonce(90),request)==Status::OutOfOrder,"probe needs mutual handshake");confirmed(p,r);require(t.begin(r,7,nonce(90),request)==Status::RoleMismatch,"only producer sends probe");require(t.begin(p,7,nonce(90),request)==Status::Ok&&!t.observed(),"fresh probe prepared");
    std::array<std::uint8_t,ProbeBytes> payload{};require(encodeProbe(request,payload)==Status::Ok,"encode probe");
    const auto prefix=hex("53 46 50 31 01 00 00 00 01 00 00 00 50 00 00 00");require(std::equal(prefix.begin(),prefix.end(),payload.begin())&&payload[48]==7&&payload[49]==0&&payload[56]==90&&payload[71]==105,"probe wire independent offsets");
    ProbeMessage decoded;require(decodeProbe(view(payload),decoded)==Status::Ok&&decoded.sequence==7,"decode coded-AU probe bytes");ProbeMessage echo;
    auto wrong=channel();wrong.authenticated=false;require(makeProbeEcho(r,decoded,wrong,echo)==Status::Unauthenticated,"no unauthenticated echo");require(makeProbeEcho(p,decoded,channel(),echo)==Status::RoleMismatch,"wrong-role echo");require(makeProbeEcho(r,decoded,channel(),echo)==Status::Ok,"receiver echoes observed probe");
    auto changed=echo;changed.sequence++;require(encodeProbe(changed,payload)==Status::Ok&&t.acceptEcho(p,view(payload),channel())==Status::SessionMismatch&&!t.observed(),"wrong sequence rejected");changed=echo;changed.freshToken[0]^=1;require(encodeProbe(changed,payload)==Status::Ok&&t.acceptEcho(p,view(payload),channel())==Status::SessionMismatch,"wrong probe token rejected");changed=echo;changed.receiverNonce[0]^=1;require(encodeProbe(changed,payload)==Status::Ok&&t.acceptEcho(p,view(payload),channel())==Status::SessionMismatch,"wrong receiver generation rejected");
    require(encodeProbe(echo,payload)==Status::Ok&&t.acceptEcho(p,view(payload),channel())==Status::Ok&&t.observed(),"fresh exact roundtrip");require(t.acceptEcho(p,view(payload),channel())==Status::Duplicate,"duplicate echo stable");require(t.begin(p,7,nonce(120),request)==Status::InvalidArgument&&t.observed(),"sequence replay rejected without state");require(t.begin(p,8,nonce(90),request)==Status::InvalidArgument,"token replay rejected");require(t.begin(p,8,nonce(120),request)==Status::Ok&&!t.observed(),"new probe clears prior observation");require(t.acceptEcho(p,view(payload),channel())==Status::SessionMismatch,"old echo cannot satisfy new probe");
    auto bad=payload;bad[72]=1;const auto saved=decoded;require(decodeProbe(view(bad),decoded)==Status::Malformed&&decoded.sequence==saved.sequence&&decoded.freshToken==saved.freshToken,"reserved probe bytes no output change");p.reset();require(t.acceptEcho(p,view(payload),channel())!=Status::Ok,"disconnect revokes old echo authority");t.reset();require(!t.observed(),"probe reset");
}
}
int main(){try{goldens();invalidState();compressedCoverageNegotiation();probe();std::cout<<checks<<" stream bridge checks passed\n";return EXIT_SUCCESS;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}}
