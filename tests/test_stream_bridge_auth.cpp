#include "../bridge/stream_bridge_auth.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
using namespace stream_bridge;
namespace {
unsigned checks=0;
void require(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
template<std::size_t N> Bytes bytes(const std::array<std::uint8_t,N>& a){return {a.data(),a.size()};}
Bytes bytes(const std::vector<std::uint8_t>& a){return {a.data(),a.size()};}
Bytes text(const char* s){return {reinterpret_cast<const std::uint8_t*>(s),std::strlen(s)};}
Nonce nonce(unsigned base){Nonce n{};for(unsigned i=0;i<n.size();++i)n[i]=static_cast<std::uint8_t>(base+i);return n;}
Pairing pair(Role role){Pairing p;p.role=role;p.pairId=nonce(1);for(unsigned i=0;i<p.key.size();++i)p.key[i]=static_cast<std::uint8_t>(i);return p;}
VideoStreamContext stream(){return {nonce(170),41};}
VideoSourceEvidence evidence(){return {stream(),true};}
std::vector<std::uint8_t> hex(const char* s){std::vector<std::uint8_t> out;std::string str(s);for(std::size_t i=0;i<str.size();i+=2)out.push_back(static_cast<std::uint8_t>(std::stoul(str.substr(i,2),nullptr,16)));return out;}
AuthPayload hello(Role role){Message m;m.role=role;m.buildId=role==Role::Producer?709:710;m.senderNonce=nonce(role==Role::Producer?33:65);AuthPayload b{};require(encode(m,b)==Status::Ok,"valid HELLO fixture");return b;}
AuthEnvelope sealed(AuthSession& session,const AuthPayload& p){AuthEnvelope out{};require(session.seal(bytes(p),out)==AuthStatus::Ok,"seal valid payload");return out;}
void put(std::uint8_t* p,std::uint64_t v,unsigned count){for(unsigned i=0;i<count;++i)p[i]=static_cast<std::uint8_t>(v>>(8*i));}
void resign(AuthEnvelope& envelope,const Pairing& p){
    // Independent envelope surgery for authenticated-invalid negative controls.
    const char domain[]="SFNP-STREAM-BRIDGE-AUTH-V1";
    std::array<std::uint8_t,sizeof(domain)-1+33> kdf{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(domain),sizeof(domain)-1,kdf.begin());
    std::copy(envelope.begin()+16,envelope.begin()+48,kdf.begin()+sizeof(domain)-1);kdf.back()=envelope[8];
    AuthTag dk{},tag{};require(hmacSha256(bytes(p.key),bytes(kdf),dk)==AuthStatus::Ok,"negative-control KDF");
    require(hmacSha256(bytes(dk),{envelope.data(),144},tag)==AuthStatus::Ok,"negative-control tag");
    std::copy(tag.begin(),tag.end(),envelope.begin()+144);secureZero(dk.data(),dk.size());
}
void unchanged(AuthSession& s,Bytes wire,AuthStatus expected,const char* label){
    AuthPayload output{};output.fill(0xab);const auto saved=output;
    VerifiedChannel ch{false,nonce(211)};const auto old=ch;
    const auto rx=s.receivedSequence(),tx=s.sentSequence();const auto binding=s.binding();const bool peer=s.peerVerified(),bound=s.bound();
    require(s.openControl(wire,output,ch)==expected,label);
    require(output==saved&&ch.authenticated==old.authenticated&&ch.binding==old.binding,"rejection leaves outputs unchanged");
    require(rx==s.receivedSequence()&&tx==s.sentSequence()&&binding==s.binding()&&peer==s.peerVerified()&&bound==s.bound(),"rejection leaves authority unchanged");
}
void hmacVectors(){
    struct Vector{std::vector<std::uint8_t> key,data;const char* digest;};
    const auto stringBytes=[](const char* s){const auto b=text(s);return std::vector<std::uint8_t>(b.data,b.data+b.size);};
    std::vector<std::uint8_t> ordered(25);for(unsigned i=0;i<25;++i)ordered[i]=static_cast<std::uint8_t>(i+1);
    const Vector cases[]={
        {std::vector<std::uint8_t>(20,0x0b),stringBytes("Hi There"),"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {stringBytes("Jefe"),stringBytes("what do ya want for nothing?"),"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {std::vector<std::uint8_t>(20,0xaa),std::vector<std::uint8_t>(50,0xdd),"773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
        {ordered,std::vector<std::uint8_t>(50,0xcd),"82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
        {std::vector<std::uint8_t>(131,0xaa),stringBytes("Test Using Larger Than Block-Size Key - Hash Key First"),"60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {std::vector<std::uint8_t>(131,0xaa),stringBytes("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm."),"9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"}
    };
    for(const auto& v:cases){AuthTag output{};require(hmacSha256(bytes(v.key),bytes(v.data),output)==AuthStatus::Ok,"RFC4231 HMAC accepted");const auto expected=hex(v.digest);require(std::equal(output.begin(),output.end(),expected.begin()),"RFC4231 exact SHA256 tag");}
    AuthTag result{};result.fill(0x96);const auto saved=result;
    for(const auto& input:{Bytes{nullptr,1},Bytes{nullptr,1025}})
        require(hmacSha256(input,text("abc"),result)==AuthStatus::InvalidArgument&&result==saved,"invalid HMAC key transactional");
    require(hmacSha256(text("key"),{nullptr,1},result)==AuthStatus::InvalidArgument&&result==saved,"null HMAC message transactional");
    require(hmacSha256(text("key"),{nullptr,4097},result)==AuthStatus::InvalidArgument&&result==saved,"oversize HMAC message transactional");
    const std::vector<std::uint8_t> longKey(1025,7),longMessage(4097,9);
    require(hmacSha256(bytes(longKey),text("abc"),result)==AuthStatus::InvalidArgument&&result==saved,"actual key size cap enforced");
    require(hmacSha256(text("abc"),bytes(longMessage),result)==AuthStatus::InvalidArgument&&result==saved,"actual message size cap enforced");
    require(hmacSha256({nullptr,0},{nullptr,0},result)==AuthStatus::Ok,"empty HMAC inputs supported");
    const auto empty=hex("b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    require(std::equal(result.begin(),result.end(),empty.begin()),"independent empty key/message tag");
}
void bootstrapAndGoldens(){
    static_assert(AuthEnvelopeBytes==176&&AuthEnvelopeBytes<=256,"bounded owned SEI");
    static_assert(!std::is_copy_constructible<AuthSession>::value&&!std::is_move_constructible<AuthSession>::value,"secret owner cannot be copied or moved");
    AuthSession p,r;require(p.prepareProducer(pair(Role::Producer),nonce(101))==AuthStatus::Ok,"paired producer prepare");
    require(r.prepareReceiver(pair(Role::Receiver),stream())==AuthStatus::Ok,"receiver prepare before binding");
    LocalSendAuthority own{};require(p.localSendAuthority(own)==AuthStatus::Ok&&own.channel.authenticated&&own.channel.binding==nonce(101)&&!p.peerVerified(),"local authority is not peer confirmation");
    LocalSendAuthority none{{false,nonce(220)}};const auto saved=none;
    require(r.localSendAuthority(none)==AuthStatus::NotBound&&none.channel.binding==saved.channel.binding&&!none.channel.authenticated,"unbound receiver cannot sign/start");
    const auto ph=hello(Role::Producer);const auto envelope=sealed(p,ph);
    // Literal full envelope independently generated with Python stdlib hmac /
    // hashlib and struct.pack. Header fields are also checked independently.
    const auto golden=hex("5346413101000000010040005000b0000102030405060708090a0b0c0d0e0f10"
        "65666768696a6b6c6d6e6f707172737401000000000000000000000000000000"
        "53464231010000000101000050000000c5020000000000000b00000000000000"
        "2122232425262728292a2b2c2d2e2f3000000000000000000000000000000000"
        "00000000000000000b0000000000000031efc92fe30711cc2f8a15dfc3167a21"
        "4822eecf7f163acdf5615b9c0e4316a5");
    require(golden.size()==176&&std::equal(envelope.begin(),envelope.end(),golden.begin()),"independent full envelope golden");
    require(envelope[8]==1&&envelope[10]==64&&envelope[12]==80&&envelope[14]==176&&envelope[48]==1,"hand-checked header offsets");
    unchanged(r,bytes(envelope),AuthStatus::NotBound,"raw TCP cannot bootstrap");
    AuthPayload output{};output.fill(0x77);const auto old=output;VerifiedChannel ch{false,nonce(211)};const auto oldCh=ch;
    auto proof=evidence();proof.currentCompleteAccessUnit=false;
    require(r.bootstrapFromVideo(bytes(envelope),proof,output,ch)==AuthStatus::SourceUnproved,"incomplete AU cannot bootstrap");
    proof=evidence();proof.stream.generation++;
    require(r.bootstrapFromVideo(bytes(envelope),proof,output,ch)==AuthStatus::SourceUnproved,"wrong native generation cannot bootstrap");
    proof=evidence();proof.stream.connection[0]^=1;
    require(r.bootstrapFromVideo(bytes(envelope),proof,output,ch)==AuthStatus::SourceUnproved,"other video stream cannot bootstrap");
    require(output==old&&ch.binding==oldCh.binding&&!ch.authenticated&&!r.bound()&&!r.peerVerified()&&r.receivedSequence()==0,"bootstrap refusals transactional");
    auto corrupt=envelope;corrupt[160]^=1;
    require(r.bootstrapFromVideo(bytes(corrupt),evidence(),output,ch)==AuthStatus::BadMac&&!r.bound(),"paired key MAC required before binding");
    require(r.bootstrapFromVideo(bytes(envelope),evidence(),output,ch)==AuthStatus::Ok&&output==ph&&ch.authenticated&&ch.binding==nonce(101)&&r.peerVerified(),"current coded-AU HELLO establishes authenticated binding");
    require(!p.peerVerified()&&p.sentSequence()==1&&r.receivedSequence()==1,"local write does not authenticate remote peer");
    require(r.bootstrapFromVideo(bytes(envelope),evidence(),output,ch)==AuthStatus::AlreadyBound,"cannot bootstrap twice");
    const auto reply=sealed(r,hello(Role::Receiver));
    require(reply[8]==2&&reply[48]==1,"independent opposite-direction sequence");
    const auto reverseTag=hex("c1475bf396582b4d37b544b44879fb144b087de9e2fd38c64481f8059a8da96f");
    require(std::equal(reply.begin()+144,reply.end(),reverseTag.begin()),"independent receiver-direction tag");
    require(p.openControl(bytes(reply),output,ch)==AuthStatus::Ok&&p.peerVerified()&&output==hello(Role::Receiver),"authenticated reverse response");
    unchanged(p,bytes(reply),AuthStatus::Replay,"same packet cannot be replayed");
    const auto next=sealed(r,hello(Role::Receiver));require(p.openControl(bytes(next),output,ch)==AuthStatus::Ok&&p.receivedSequence()==2,"new reverse packet accepted");
    unchanged(p,bytes(reply),AuthStatus::Replay,"out of order earlier packet rejected");
    const auto signedBefore=p.sentSequence();AuthEnvelope unmodified{};unmodified.fill(0xbb);const auto sentinel=unmodified;
    require(p.seal(bytes(hello(Role::Receiver)),unmodified)==AuthStatus::PayloadInvalid&&unmodified==sentinel&&p.sentSequence()==signedBefore,"cannot sign reflected peer role");
}
void authenticatedNegatives(){
    AuthSession p,r;require(p.prepareProducer(pair(Role::Producer),nonce(101))==AuthStatus::Ok,"negative producer");
    require(r.prepareReceiver(pair(Role::Receiver),stream())==AuthStatus::Ok,"negative receiver");
    const auto first=sealed(p,hello(Role::Producer));AuthPayload output{};VerifiedChannel ch{};
    require(r.bootstrapFromVideo(bytes(first),evidence(),output,ch)==AuthStatus::Ok,"negative bootstrap");
    const auto good=sealed(p,hello(Role::Producer));
    for(std::size_t size=0;size<AuthEnvelopeBytes;++size)unchanged(r,{good.data(),size},AuthStatus::Malformed,"all truncated envelope lengths refused");
    unchanged(r,{nullptr,176},AuthStatus::InvalidArgument,"null control input refused");
    auto extra=std::vector<std::uint8_t>(good.begin(),good.end());extra.push_back(0);
    unchanged(r,bytes(extra),AuthStatus::Malformed,"extra byte refused");
    for(std::size_t at=0;at<good.size();++at){
        auto bad=good;bad[at]^=0x80;
        AuthPayload unchangedOutput{};unchangedOutput.fill(0xee);const auto saved=unchangedOutput;
        VerifiedChannel unchangedChannel{false,nonce(220)};const auto rx=r.receivedSequence();
        require(r.openControl(bytes(bad),unchangedOutput,unchangedChannel)!=AuthStatus::Ok,"each envelope byte integrity protected");
        require(unchangedOutput==saved&&!unchangedChannel.authenticated&&unchangedChannel.binding==nonce(220)&&r.receivedSequence()==rx,"byte corruption cannot mutate outputs/replay state");
    }
    auto bad=good;bad[8]=2;resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::RoleMismatch,"authenticated reflected direction rejected");
    bad=good;bad[16]^=1;resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::PairMismatch,"authenticated wrong pairing rejected");
    bad=good;bad[32]^=1;resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::BindingMismatch,"authenticated other binding rejected");
    bad=good;put(bad.data()+48,0,8);resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::Replay,"authenticated zero sequence rejected");
    bad=good;bad[64+9]=2;resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::PayloadInvalid,"inner role must match authenticated outer direction");
    bad=good;bad[64+4]=2;resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::PayloadInvalid,"authenticated incompatible inner protocol rejected");
    bad=good;bad[64]=0;resign(bad,pair(Role::Producer));unchanged(r,bytes(bad),AuthStatus::PayloadInvalid,"authenticated arbitrary payload rejected");
    require(r.openControl(bytes(good),output,ch)==AuthStatus::Ok&&r.receivedSequence()==2,"all invalid attempts leave valid sequence usable");
    bad=good;put(bad.data()+48,~std::uint64_t(0),8);resign(bad,pair(Role::Producer));
    require(r.openControl(bytes(bad),output,ch)==AuthStatus::Ok&&r.receivedSequence()==~std::uint64_t(0),"maximum inbound sequence does not overflow");
    const auto subsequent=sealed(p,hello(Role::Producer));unchanged(r,bytes(subsequent),AuthStatus::Replay,"no wrap after maximum inbound sequence");
}
void firstMessageAndReset(){
    AuthSession idle;AuthEnvelope idleWire{};idleWire.fill(0xd5);const auto idleSaved=idleWire;
    LocalSendAuthority idleAuthority{{false,nonce(222)}};
    require(idle.seal(bytes(hello(Role::Producer)),idleWire)==AuthStatus::NotPrepared&&idleWire==idleSaved,"unpaired sender cannot sign");
    require(idle.localSendAuthority(idleAuthority)==AuthStatus::NotPrepared&&!idleAuthority.channel.authenticated&&idleAuthority.channel.binding==nonce(222),"unpaired local authority unchanged");
    AuthSession p,r;require(p.prepareProducer(pair(Role::Producer),nonce(101))==AuthStatus::Ok,"reset producer");
    require(r.prepareReceiver(pair(Role::Receiver),stream())==AuthStatus::Ok,"reset receiver");
    auto first=sealed(p,hello(Role::Producer));AuthPayload output{};output.fill(0x66);VerifiedChannel channel{};
    auto nonHello=first;Message ack;ack.kind=Kind::Ack;ack.role=Role::Producer;ack.buildId=709;ack.senderNonce=nonce(33);ack.peerNonce=nonce(65);ack.selected=RequiredCapabilities;
    AuthPayload ackWire{};require(encode(ack,ackWire)==Status::Ok,"ACK bootstrap negative payload");
    std::copy(ackWire.begin(),ackWire.end(),nonHello.begin()+64);resign(nonHello,pair(Role::Producer));
    require(r.bootstrapFromVideo(bytes(nonHello),evidence(),output,channel)==AuthStatus::PayloadInvalid&&!r.bound(),"MAC-valid ACK cannot bootstrap");
    ProbeMessage probe;probe.producerNonce=nonce(33);probe.receiverNonce=nonce(65);probe.sequence=5;probe.freshToken=nonce(80);
    AuthPayload probeWire{};require(encodeProbe(probe,probeWire)==Status::Ok,"probe bootstrap negative payload");
    std::copy(probeWire.begin(),probeWire.end(),nonHello.begin()+64);resign(nonHello,pair(Role::Producer));
    require(r.bootstrapFromVideo(bytes(nonHello),evidence(),output,channel)==AuthStatus::PayloadInvalid&&!r.bound(),"MAC-valid probe cannot bootstrap");
    require(r.bootstrapFromVideo(bytes(first),evidence(),output,channel)==AuthStatus::Ok,"HELLO after invalid bootstrap");
    const auto pBefore=p.sentSequence();auto invalid=pair(Role::Producer);invalid.key={};
    require(p.prepareProducer(invalid,nonce(120))==AuthStatus::InvalidArgument&&p.binding()==nonce(101)&&p.sentSequence()==pBefore,"invalid producer prepare preserves active session");
    invalid=pair(Role::Receiver);invalid.pairId={};require(r.prepareReceiver(invalid,stream())==AuthStatus::InvalidArgument&&r.bound(),"invalid receiver prepare preserves active session");
    r.reset();p.reset();require(!r.prepared()&&!r.bound()&&!r.peerVerified()&&!p.prepared()&&!p.bound()&&p.sentSequence()==0&&r.receivedSequence()==0,"loss clears all live authority");
    unchanged(r,bytes(first),AuthStatus::NotPrepared,"reset cannot receive stale data");
    require(p.prepareProducer(pair(Role::Producer),nonce(101))==AuthStatus::InvalidArgument&&!p.prepared(),"previous producer binding cannot be reused after reset");
    auto newStream=stream();newStream.generation++;
    require(r.prepareReceiver(pair(Role::Receiver),newStream)==AuthStatus::Ok,"new native generation prepared");
    auto newProof=VideoSourceEvidence{newStream,true};
    require(r.bootstrapFromVideo(bytes(first),newProof,output,channel)==AuthStatus::BindingMismatch&&!r.bound(),"previous authenticated binding cannot preempt reconnect");
    require(p.prepareProducer(pair(Role::Producer),nonce(120))==AuthStatus::Ok,"fresh producer binding allowed");
    const auto fresh=sealed(p,hello(Role::Producer));
    require(r.bootstrapFromVideo(bytes(fresh),evidence(),output,channel)==AuthStatus::SourceUnproved,"old AU context cannot bootstrap new native generation");
    require(r.bootstrapFromVideo(bytes(fresh),newProof,output,channel)==AuthStatus::Ok&&r.binding()==nonce(120),"fresh bound reconnect");
    const auto probeEnvelope=sealed(p,probeWire);
    require(r.openControl(bytes(probeEnvelope),output,channel)==AuthStatus::Ok&&output==probeWire,"probe travels authenticated but grants no image permission");
    probe.kind=ProbeKind::Echo;require(encodeProbe(probe,probeWire)==Status::Ok,"echo payload");
    const auto echo=sealed(r,probeWire);require(p.openControl(bytes(echo),output,channel)==AuthStatus::Ok&&output==probeWire,"reverse probe echo authenticated");
    AuthEnvelope sentinel{};sentinel.fill(0xee);const auto before=sentinel;
    require(p.seal({nullptr,80},sentinel)==AuthStatus::PayloadInvalid&&sentinel==before,"null sealing payload transactional");
    require(p.seal({probeWire.data(),79},sentinel)==AuthStatus::PayloadInvalid&&sentinel==before,"short sealing payload transactional");
}
void erase(){
    std::array<std::uint8_t,129> secret{};secret.fill(0xa3);secureZero(secret.data(),secret.size());
    require(std::all_of(secret.begin(),secret.end(),[](auto b){return b==0;}),"volatile wipe clears complete caller buffer");
    secureZero(nullptr,5);secureZero(secret.data(),0);
    AuthSession s;auto p=pair(Role::Producer);p.key.fill(0xd7);require(s.prepareProducer(p,nonce(101))==AuthStatus::Ok,"wipe prepared key");
    const auto patternExists=[&](){const auto* begin=reinterpret_cast<const std::uint8_t*>(&s);return std::search_n(begin,begin+sizeof(s),32,std::uint8_t(0xd7))!=begin+sizeof(s);};
    require(patternExists(),"key erasure fixture actually contains key before reset");s.reset();
    require(!patternExists(),"reset removes stored secret bytes");
    alignas(AuthSession) std::array<std::uint8_t,sizeof(AuthSession)> storage{};
    auto* placed=new(storage.data()) AuthSession;
    require(placed->prepareProducer(p,nonce(101))==AuthStatus::Ok,"destructor key fixture prepared");
    require(std::search_n(storage.begin(),storage.end(),32,std::uint8_t(0xd7))!=storage.end(),"destructor fixture actually owns key");
    placed->~AuthSession();
    require(std::all_of(storage.begin(),storage.end(),[](auto b){return b==0;}),"destructor volatile erasure covers owned object storage");
}
}
int main(){try{hmacVectors();bootstrapAndGoldens();authenticatedNegatives();firstMessageAndReset();erase();std::cout<<checks<<" stream bridge authentication checks passed\n";return EXIT_SUCCESS;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}}
