#include "pc/stream-bridge-sender.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using namespace stream_bridge;
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
Nonce nonce(unsigned value){Nonce n{};n[0]=static_cast<std::uint8_t>(value);return n;}
VerifiedChannel channel(){return {true,nonce(99)};}
LocalConfig config(Role role,unsigned n){LocalConfig c{};c.role=role;c.nonce=nonce(n);c.buildId=n;return c;}
VerifiedSource source(){VerifiedSource s{};s.verified=s.ordinaryStream=s.completeAccessUnit=true;
 s.codec=VerifiedSource::Codec::HevcAnnexB;s.moduleSha256[0]=1;s.apiVersion=0x7102000b;s.encoderGeneration=1;return s;}
const std::vector<std::uint8_t> au{0,0,0,1,0x26,1,0x80,0x12,0x34};
Bytes bytes(const std::vector<std::uint8_t>& b){return {b.data(),b.size()};}
Message unpack(Bytes b){std::vector<std::uint8_t> p;check(inspectOwned(b,p)==SeiStatus::Ok,"owned SEI inspect");Message m{};check(decode(bytes(p),m)==Status::Ok,"decode owned message");return m;}
void exchange(SenderAdapter& sender,Handshake& receiver){
 for(unsigned i=0;i<3;++i){auto prepared=sender.prepareDiscovery(bytes(au),source());check(prepared.appended,"producer discovery appended");
  auto message=unpack(prepared.output);check(sender.complete(prepared.ticket,true,channel())==Status::Ok,"actual AU submit");
  std::array<std::uint8_t,MessageBytes> wire{};check(encode(message,wire)==Status::Ok,"encode producer");
  check(receiver.receive({wire.data(),wire.size()},channel())==Status::Ok,"receiver consume");
  Message reply{};check(receiver.next(reply)==Status::Ok,"receiver next");check(encode(reply,wire)==Status::Ok,"encode reply");
  check(receiver.markSent(reply,channel())==Status::Ok,"receiver actual send");
  check(sender.receiveControl({wire.data(),wire.size()},channel())==Status::Ok,"producer reply");}
 check(sender.mutualConfirmed()&&receiver.confirmed(),"both commits confirmed");
}
void notify(SenderStatus,void* p){++*static_cast<unsigned*>(p);}
bool seal(Bytes plain,std::vector<std::uint8_t>& output,void* context){
 if(!*static_cast<bool*>(context))return false;
 output.assign(16,0x5a);output.insert(output.end(),plain.data,plain.data+plain.size);return true;
}
void tests(){
 SenderAdapter sender;auto p=sender.prepareDiscovery(bytes(au),source());
 check(!p.appended&&p.output.data==au.data()&&p.output.size==au.size(),"inactive exact original bytes and pointer");
 auto unverified=source();unverified.verified=false;
 check(sender.request(config(Role::Producer,1),channel(),unverified)==SenderStatus::SourceUnverified,"unverified source refused");
 auto c=config(Role::Producer,1);c.offered|=(1ull<<63);
 check(sender.request(c,channel(),source())==SenderStatus::InvalidRequest,"unknown capability bits refused");
 auto known=config(Role::Producer,77);known.offered=known.required=KnownCapabilities;SenderAdapter capable;
 check(capable.request(known,channel(),source())==SenderStatus::WaitingPeer&&!capable.coveragePermitted(),"known route capabilities negotiate without image permission");
 auto unauth=channel();unauth.authenticated=false;
 check(sender.request(config(Role::Producer,1),unauth,source())==SenderStatus::ControlRejected,"no auth no writes");
 check(sender.request(config(Role::Producer,1),channel(),source())==SenderStatus::WaitingPeer,"explicit known requested");
 p=sender.prepareDiscovery(bytes(au),source());check(p.appended&&!sender.mutualConfirmed(),"HELLO discovery before mutual");
 auto hello=unpack(p.output);check(hello.kind==Kind::Hello,"hello payload");
 check(!sender.coveragePermitted(),"HELLO never alpha permission");
 std::vector<std::uint8_t> restored;check(removeOwned(p.output,restored)==SeiStatus::Ok&&restored==au,"all original AU bytes exact");
 auto copy=std::vector<std::uint8_t>(p.output.data,p.output.data+p.output.size);
 auto second=sender.prepareDiscovery(bytes(au),source());check(!second.appended&&second.output.data==au.data(),"single pending submission");
 check(sender.complete(p.ticket+1,true,channel())==Status::SessionMismatch,"wrong ticket cannot free lease");
 check(std::memcmp(copy.data(),p.output.data,copy.size())==0,"pending unchanged");
 check(sender.complete(p.ticket,false,channel())==Status::Ok,"canceled constructed AU");
 auto retry=sender.prepareDiscovery(bytes(au),source());check(retry.appended&&unpack(retry.output).kind==Kind::Hello,"canceled HELLO unsent");
 sender.stop();check(!sender.requested(),"stop revokes authority");
 check(std::memcmp(copy.data(),retry.output.data,copy.size())==0,"stop retains borrowed bytes");
 check(sender.complete(retry.ticket,true,channel())==Status::OutOfOrder,"late completion not handshake advancement");
 check(sender.request(config(Role::Producer,2),channel(),source())==SenderStatus::WaitingPeer,"new explicit epoch");
 auto changed=source();++changed.encoderGeneration;p=sender.prepareDiscovery(bytes(au),changed);
 check(!p.appended&&p.output.data==au.data()&&!sender.requested(),"source generation change revokes byte exact");
 check(sender.request(config(Role::Producer,3),channel(),source())==SenderStatus::WaitingPeer,"fresh epoch after generation change");
 std::vector<std::uint8_t> malformed{1,2,3};p=sender.prepareDiscovery(bytes(malformed),source());
 check(!p.appended&&p.output.data==malformed.data(),"malformed original exact");
 Nonce token=nonce(20);p=sender.prepareProbe(bytes(au),source(),1,token);check(!p.appended,"probe before mutual refused");
 Handshake receiver;check(receiver.begin(config(Role::Receiver,4),channel())==Status::Ok,"receiver begin");exchange(sender,receiver);
 check(!sender.coveragePermitted(),"mutual still no coverage permission");
 p=sender.prepareProbe(bytes(au),source(),1,token);check(p.appended,"mutual probe append");
 std::vector<std::uint8_t> payload;check(inspectOwned(p.output,payload)==SeiStatus::Ok,"probe read from actual AU");
 ProbeMessage probe{};check(decodeProbe(bytes(payload),probe)==Status::Ok,"probe payload parse");
 ProbeMessage echo{};check(makeProbeEcho(receiver,probe,channel(),echo)==Status::Ok,"receiver echoes actual AU proof");
 std::array<std::uint8_t,ProbeBytes> wire{};check(encodeProbe(echo,wire)==Status::Ok,"echo encoding");
 check(sender.receiveEcho({wire.data(),wire.size()},channel())==Status::OutOfOrder,"unsubmitted probe not observed");
 check(sender.complete(p.ticket,true,channel())==Status::Ok,"probe actual submit");
 check(sender.receiveEcho({wire.data(),wire.size()},unauth)==Status::Unauthenticated,"unauth echo refused");
 check(!sender.probeObserved(),"invalid echo no observation");
 check(sender.receiveEcho({wire.data(),wire.size()},channel())==Status::Ok&&sender.probeObserved(),"authenticated exact roundtrip");
 check(!sender.coveragePermitted(),"transport observed never MR proof");
 p=sender.prepareProbe(bytes(au),source(),1,nonce(21));check(!p.appended,"duplicate sequence refused");
 bool permitSeal=true;SenderAdapter sealed;
 check(sealed.request(config(Role::Producer,12),channel(),source(),{seal,&permitSeal})==SenderStatus::WaitingPeer,"explicit payload sealer port");
 p=sealed.prepareDiscovery(bytes(au),source());check(p.appended,"sealed discovery appended");
 payload.clear();check(inspectOwned(p.output,payload)==SeiStatus::Ok&&payload.size()==96&&payload[0]==0x5a,"SEI carries envelope rather than raw message");
 check(decode({payload.data()+16,payload.size()-16},hello)==Status::Ok&&hello.kind==Kind::Hello,"sealed payload retains raw handshake");
 check(sealed.complete(p.ticket,false,channel())==Status::Ok,"canceled sealed payload retains unsent state");
 permitSeal=false;p=sealed.prepareDiscovery(bytes(au),source());
 check(!p.appended&&p.output.data==au.data()&&p.output.size==au.size(),"sealer failure exact original AU");
 unsigned notifications=0;SenderAdapter bounded(notify,&notifications);
 check(bounded.request(config(Role::Producer,7),channel(),source())==SenderStatus::WaitingPeer,"bounded notifications start");
 for(unsigned i=0;i<200;++i){bounded.prepareDiscovery(bytes(malformed),source());bounded.prepareProbe(bytes(au),source(),1,token);}
 check(notifications<=32,"bounded deduplicated events");bounded.stop();check(notifications<=33,"off remains observable");
}
}
int main(){try{tests();std::printf("sender adapter: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"sender failure after %u: %s\n",checks,e.what());return 1;}}
