#include "../bridge/stream_bridge_auth.h"
#include "../pc/stream-bridge-sender.h"
#include "../receiver/stream_bridge_receiver.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
namespace sb=stream_bridge;
namespace tx=stream_bridge::sender;
namespace rx=stream_bridge_receiver;
namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
sb::Nonce nonce(unsigned base){sb::Nonce n{};for(unsigned i=0;i<n.size();++i)n[i]=static_cast<std::uint8_t>(base+i);return n;}
sb::Bytes bytes(const std::vector<std::uint8_t>& b){return {b.data(),b.size()};}
template<std::size_t N> sb::Bytes bytes(const std::array<std::uint8_t,N>& b){return {b.data(),b.size()};}
// Structurally valid Annex-B container, not a complete codec/Steam fixture.
// An optional argv[1] supplies an independently encoded complete HEVC AU.
std::vector<std::uint8_t> ordinary{0,0,0,1,0x46,0x01,0x50,0,0,1,0x26,0x01,0x80,0x12,0x34};
sb::Pairing pairing(sb::Role role){sb::Pairing p;p.role=role;p.pairId=nonce(1);for(unsigned i=0;i<p.key.size();++i)p.key[i]=static_cast<std::uint8_t>(i+3);return p;}
sb::LocalConfig config(sb::Role role){sb::LocalConfig c;c.role=role;c.buildId=role==sb::Role::Producer?709:1710;c.nonce=nonce(role==sb::Role::Producer?33:65);return c;}
tx::VerifiedSource source(){tx::VerifiedSource s;s.verified=s.ordinaryStream=s.completeAccessUnit=true;s.codec=tx::VerifiedSource::Codec::HevcAnnexB;s.moduleSha256.fill(0x35);s.apiVersion=0x7102000b;s.encoderGeneration=27;return s;}
struct SealState{sb::AuthSession* auth=nullptr;unsigned calls=0;bool available=true;};
bool seal(sb::Bytes plain,std::vector<std::uint8_t>& output,void* context){
    auto& state=*static_cast<SealState*>(context);++state.calls;
    if(!state.available)return false;
    sb::AuthEnvelope envelope{};if(state.auth->seal(plain,envelope)!=sb::AuthStatus::Ok)return false;
    output.assign(envelope.begin(),envelope.end());return true;
}
struct VerifyState{sb::AuthSession* auth=nullptr;unsigned calls=0,bootstraps=0;sb::AuthStatus status=sb::AuthStatus::NotPrepared;};
bool verify(sb::Bytes payload,const rx::AccessUnitContext& context,sb::AuthPayload& plain,
            sb::VerifiedChannel& channel,void* owner) noexcept {
    auto& state=*static_cast<VerifyState*>(owner);++state.calls;
    if(!state.auth->bound()){
        // This is deterministic caller evidence in a software fixture, not a
        // discovered native connection/provider. Actual runtime must prove it.
        const sb::VideoSourceEvidence proof{{context.authority.session,context.authority.streamGeneration},
            context.sourceBoundaryVerified&&context.completeAnnexBAccessUnit};
        state.status=state.auth->bootstrapFromVideo(payload,proof,plain,channel);
        if(state.status==sb::AuthStatus::Ok)++state.bootstraps;
    }else state.status=state.auth->openControl(payload,plain,channel);
    return state.status==sb::AuthStatus::Ok;
}
std::array<std::uint8_t,sb::MessageBytes> wire(const sb::Message& message){sb::AuthPayload b{};require(sb::encode(message,b)==sb::Status::Ok,"encode actual outbound handshake");return b;}
sb::AuthPayload wire(const sb::ProbeMessage& message){sb::AuthPayload b{};require(sb::encodeProbe(message,b)==sb::Status::Ok,"encode actual probe/echo");return b;}
struct Rig{
    sb::AuthSession producerAuth,receiverAuth;
    tx::SenderAdapter sender;
    rx::Receiver receiver;
    sb::LocalSendAuthority producerAuthority{};
    SealState sealing{&producerAuth};VerifyState verifying{&receiverAuth};
    rx::Authority base{nonce(121),4,{}};
    rx::NativeFrame native{41,77};rx::Surface surface{6,809,12};
    Rig(sb::Pairing remote=pairing(sb::Role::Receiver),sb::LocalConfig receiverConfig=config(sb::Role::Receiver)){
        require(producerAuth.prepareProducer(pairing(sb::Role::Producer),nonce(180))==sb::AuthStatus::Ok,"producer private pairing prepared");
        require(producerAuth.localSendAuthority(producerAuthority)==sb::AuthStatus::Ok&&!producerAuth.peerVerified(),"initial local protection is not remote proof");
        require(receiverAuth.prepareReceiver(remote,{base.session,base.streamGeneration})==sb::AuthStatus::Ok,"receiver pairing with explicit native stream prepared");
        require(receiver.beginAuthenticated(base,receiverConfig,verify,&verifying),"receiver defers authenticated binding until actual video");
        require(sender.request(config(sb::Role::Producer),producerAuthority.channel,source(),{seal,&sealing})==tx::SenderStatus::WaitingPeer,"sender explicit discovery uses real auth sealer");
        sb::Message noReply;require(receiver.nextOutbound(noReply)==sb::Status::NoMessage,"unbound receiver has no outbound HELLO");
    }
    rx::Authority authority() const {
        auto a=base;sb::LocalSendAuthority local;
        if(receiverAuth.localSendAuthority(local)==sb::AuthStatus::Ok)a.channel=local.channel;
        return a;
    }
    rx::AccessUnitContext context() const{return {authority(),native,true,true};}
    rx::AccessUnitResult observe(const std::vector<std::uint8_t>& coded,const rx::AccessUnitContext& ctx){
        const auto saved=coded;const auto result=receiver.observeAccessUnit(bytes(coded),ctx);
        require(result.original.data==coded.data()&&result.original.size==coded.size(),"receiver always returns exact original complete AU pointer and length");
        require(coded==saved,"receiver never edits any AU/SEI/VCL byte");return result;
    }
    rx::AccessUnitResult observe(const std::vector<std::uint8_t>& coded){return observe(coded,context());}
    std::vector<std::uint8_t> prepareAndSubmit(bool probe=false){
        const auto prepared=probe?sender.prepareProbe(bytes(ordinary),source(),17,nonce(97)):
            sender.prepareDiscovery(bytes(ordinary),source());
        require(prepared.appended&&prepared.ticket!=0,"sender builds signed metadata in actual provided AU");
        std::vector<std::uint8_t> extracted,restored;
        require(sb::inspectOwned(prepared.output,extracted)==sb::SeiStatus::Ok&&extracted.size()==sb::AuthEnvelopeBytes,"SEI contains exact176 authenticated envelope, not raw80");
        require(sb::removeOwned(prepared.output,restored)==sb::SeiStatus::Ok&&restored==ordinary,"every original NAL/startcode remains byte-identical");
        // Models the actual pinned consumer copy boundary. No metadata is
        // decoded and then manufactured into another AU for receiver input.
        std::vector<std::uint8_t> copied(prepared.output.data,prepared.output.data+prepared.output.size);
        require(sender.complete(prepared.ticket,true,producerAuthority.channel)==sb::Status::Ok,"actual coded-byte copy acknowledged once");
        return copied;
    }
    void reverse(sb::Kind kind,bool complete=true){
        sb::Message reply;require(receiver.nextOutbound(reply)==sb::Status::Ok&&reply.kind==kind,"receiver planned reverse action");
        const auto plain=wire(reply);sb::AuthEnvelope packet{};
        require(receiverAuth.seal(bytes(plain),packet)==sb::AuthStatus::Ok,"real HMAC authenticates reverse metadata");
        if(!complete)return;
        sb::AuthPayload received{};sb::VerifiedChannel channel{};
        require(producerAuth.openControl(bytes(packet),received,channel)==sb::AuthStatus::Ok&&received==plain,"producer authenticates exact reverse packet before decode");
        require(sender.receiveControl(bytes(received),channel)==sb::Status::Ok,"producer receives only authenticated plaintext");
        require(receiver.markOutboundSent(reply,authority())==sb::Status::Ok,"receiver records successful reverse handoff");
    }
    void exchange(){
        for(const auto kind:{sb::Kind::Hello,sb::Kind::Ack,sb::Kind::Commit}){
            const auto calls=verifying.calls;
            const auto coded=prepareAndSubmit();
            require(observe(coded).result==rx::Result::Handshake,"signed producer message consumed from original AU");
            require(verifying.calls==calls+1&&verifying.status==sb::AuthStatus::Ok,"one MAC verification per actual video message");
            if(kind==sb::Kind::Hello){require(verifying.bootstraps==1&&receiverAuth.bound()&&!sender.mutualConfirmed(),"single actual-AU bootstrap without premature confirmation");}
            if(kind==sb::Kind::Commit){require(!receiver.snapshot().peerConfirmed&&!sender.mutualConfirmed(),"unsigned/unsent local COMMIT not mutual confirmation");}
            reverse(kind);
        }
        require(sender.mutualConfirmed()&&receiver.snapshot().peerConfirmed,"both signed commitments recognized");
        require(producerAuth.peerVerified()&&receiverAuth.peerVerified()&&verifying.bootstraps==1,"both MAC-authenticated directions and one bootstrap");
        require(!sender.coveragePermitted()&&!sender.probeObserved()&&!receiver.snapshot().metadataSelected,"mutual authentication alone never permits image changes");
    }
};
void missingAndOrdinary(){
    tx::SenderAdapter absent;const auto ordinaryCopy=ordinary;
    const auto off=absent.prepareDiscovery(bytes(ordinary),source());
    require(!off.appended&&off.output.data==ordinary.data()&&off.output.size==ordinary.size()&&ordinary==ordinaryCopy,"missing sender exact opaque original fallback");
    Rig rig;for(unsigned i=0;i<12;++i)require(rig.observe(ordinary).result==rx::Result::Ordinary,"missing peer ordinary video is never rewritten");
    require(rig.verifying.calls==0&&!rig.receiverAuth.bound(),"no own SEI no authentication work or synthetic binding");
    rig.receiver.requestStatus();rx::Notice n;require(rig.receiver.pollNotice(n)&&n.code==rx::NoticeCode::SenderMissing,"missing authenticated peer useful status");
    for(unsigned i=0;i<16;++i)rig.receiver.requestStatus();
    require(!rig.receiver.pollNotice(n),"missing status bounded per session");
    const auto initial=rig.prepareAndSubmit();require(!initial.empty()&&!rig.sender.mutualConfirmed(),"one HELLO attempt does not need mutual confirmation");
    const auto noNext=rig.sender.prepareDiscovery(bytes(ordinary),source());
    require(!noNext.appended&&noNext.output.data==ordinary.data()&&noNext.status==tx::SenderStatus::NothingToSend,"no peer produces no continuous per-frame SEI retry");
    Rig noAuth;noAuth.sealing.available=false;const auto failed=noAuth.sender.prepareDiscovery(bytes(ordinary),source());
    require(!failed.appended&&failed.output.data==ordinary.data()&&failed.output.size==ordinary.size()&&failed.status==tx::SenderStatus::ControlRejected,"missing auth sealer never sends raw80 or changes video");
    require(!noAuth.producerAuth.peerVerified()&&!noAuth.sender.coveragePermitted(),"missing auth never enables image route");
}
void refusals(){
    Rig rig;const auto coded=rig.prepareAndSubmit();const auto before=rig.verifying.calls;
    auto ctx=rig.context();ctx.authority.session[0]^=1;
    require(rig.observe(coded,ctx).result==rx::Result::WrongSession,"other native stream rejected before auth");
    ctx=rig.context();++ctx.authority.streamGeneration;
    require(rig.observe(coded,ctx).result==rx::Result::WrongSession,"stale/new stream generation never inferred");
    ctx=rig.context();ctx.completeAnnexBAccessUnit=false;
    require(rig.observe(coded,ctx).result==rx::Result::UnverifiedBoundary,"incomplete AU refused before auth");
    ctx=rig.context();ctx.sourceBoundaryVerified=false;
    require(rig.observe(coded,ctx).result==rx::Result::UnverifiedBoundary,"unproved source boundary refused before auth");
    require(rig.verifying.calls==before&&!rig.receiverAuth.bound(),"wrong stream/incomplete input cannot consume sequence or bootstrap");
    std::vector<std::uint8_t> raw;require(sb::inspectOwned(bytes(coded),raw)==sb::SeiStatus::Ok,"extract actual signed metadata for corruption");
    auto tampered=raw;tampered.back()^=1;std::vector<std::uint8_t> badAu;
    require(sb::appendOwned(bytes(ordinary),bytes(tampered),badAu)==sb::SeiStatus::Ok,"container preserves deliberately invalid MAC");
    require(rig.observe(badAu).result==rx::Result::InvalidOwned&&rig.verifying.status==sb::AuthStatus::BadMac&&!rig.receiverAuth.bound(),"bad MAC cannot bind actual video receiver");
    require(rig.observe(coded).result==rx::Result::Handshake&&rig.receiverAuth.receivedSequence()==1,"correct packet after refusals still bootstraps");
    require(rig.observe(coded).result==rx::Result::InvalidOwned&&rig.verifying.status==sb::AuthStatus::Replay&&rig.receiverAuth.receivedSequence()==1,"same signed AU cannot authenticate twice");
    require(rig.verifying.bootstraps==1&&!rig.receiver.snapshot().peerConfirmed,"replay leaves single bootstrap and no false mutual state");
    auto other=pairing(sb::Role::Receiver);other.pairId[0]^=1;Rig wrongPair(other);
    require(wrongPair.observe(coded).result==rx::Result::InvalidOwned&&wrongPair.verifying.status==sb::AuthStatus::PairMismatch&&!wrongPair.receiverAuth.bound(),"wrong paired device does not recognize sender");
    other=pairing(sb::Role::Receiver);other.key[0]^=1;Rig wrongSecret(other);
    require(wrongSecret.observe(coded).result==rx::Result::InvalidOwned&&wrongSecret.verifying.status==sb::AuthStatus::BadMac&&!wrongSecret.receiverAuth.bound(),"same public pair ID without secret is insufficient");
    Rig raw80;sb::Message hello;hello.role=sb::Role::Producer;hello.buildId=709;hello.senderNonce=config(sb::Role::Producer).nonce;
    const auto plain=wire(hello);std::vector<std::uint8_t> unprotected;
    require(sb::appendOwned(bytes(ordinary),bytes(plain),unprotected)==sb::SeiStatus::Ok,"unprotected old-format discovery fixture");
    require(raw80.observe(unprotected).result==rx::Result::InvalidOwned&&raw80.verifying.status==sb::AuthStatus::Malformed&&!raw80.receiverAuth.bound(),"raw80 SEI cannot bypass authenticated entry");
    auto requirements=config(sb::Role::Receiver);requirements.offered|=sb::CapExplicitCoverage;requirements.required|=sb::CapExplicitCoverage;
    Rig incompatible(pairing(sb::Role::Receiver),requirements);
    require(incompatible.observe(coded).result==rx::Result::Incompatible&&incompatible.receiverAuth.peerVerified()&&!incompatible.receiver.snapshot().peerConfirmed,"paired authentication does not override incompatible capabilities");
    incompatible.receiver.requestStatus();rx::Notice notice;
    require(incompatible.receiver.pollNotice(notice)&&notice.code==rx::NoticeCode::SenderIncompatible,"compatible key but incompatible feature has useful bounded notice");
    incompatible.receiver.requestStatus();require(!incompatible.receiver.pollNotice(notice),"incompatible notice not repeated each frame");
}
void authenticatedProbeAndSelection(){
    Rig rig;rig.exchange();sb::ProbeMessage echo;
    require(!rig.receiver.nextEcho(echo),"mutual handshake alone cannot invent AU observation");
    const auto coded=rig.prepareAndSubmit(true);
    require(!rig.sender.probeObserved()&&!rig.receiver.nextEcho(echo),"prepared/submitted source probe is not a remote observation");
    auto incomplete=rig.context();incomplete.completeAnnexBAccessUnit=false;
    require(rig.observe(coded,incomplete).result==rx::Result::UnverifiedBoundary&&!rig.receiver.nextEcho(echo),"incomplete probe cannot produce an echo");
    require(rig.observe(coded).result==rx::Result::MetadataObserved,"signed probe observed in original coded AU");
    require(rig.receiver.snapshot().probeCount==1&&!rig.receiver.snapshot().metadataSelected&&!rig.sender.probeObserved(),"observed transport not exact selected video identity");
    require(rig.receiver.nextEcho(echo)&&echo.sequence==17&&echo.freshToken==nonce(97),"echo copies exact observed sequence and token");
    const auto plain=wire(echo);sb::AuthEnvelope signedEcho{};
    require(rig.receiverAuth.seal(bytes(plain),signedEcho)==sb::AuthStatus::Ok,"reverse probe echo has actual HMAC");
    auto corrupt=signedEcho;corrupt.back()^=1;sb::AuthPayload received{};sb::VerifiedChannel channel{};
    require(rig.producerAuth.openControl(bytes(corrupt),received,channel)==sb::AuthStatus::BadMac&&!rig.sender.probeObserved(),"unauthenticated echo cannot reach probe tracker");
    require(rig.producerAuth.openControl(bytes(signedEcho),received,channel)==sb::AuthStatus::Ok,"producer verifies echo envelope once");
    require(rig.sender.receiveEcho(bytes(received),channel)==sb::Status::Ok&&rig.sender.probeObserved(),"exact authenticated echo closes actual-AU probe roundtrip");
    require(rig.receiver.markEchoSent(echo,rig.authority()),"actual reverse echo handoff recorded");
    require(!rig.receiver.nextEcho(echo),"echo not emitted repeatedly per frame");
    rx::SelectedContext selected{rig.authority(),rig.native,rig.surface,true};
    require(!rig.receiver.selectedMetadata(selected),"MAC and probe cannot invent decoder/surface association");
    rx::DecodedContext decoded{rig.authority(),rig.native,rig.surface,false};
    require(rig.receiver.observeDecoded(decoded)==rx::Result::InvalidEvidence,"caller must prove actual decoded correspondence");
    decoded.decodedAssociationVerified=true;
    require(rig.receiver.observeDecoded(decoded)==rx::Result::MetadataObserved,"explicit exact native-to-decoded tuple recorded");
    auto wrong=selected;wrong.selectedAssociationVerified=false;require(!rig.receiver.selectedMetadata(wrong),"selected-surface authority is independently required");
    wrong=selected;++wrong.native.key;require(!rig.receiver.selectedMetadata(wrong),"another native key cannot use last received probe");
    wrong=selected;++wrong.native.generation;require(!rig.receiver.selectedMetadata(wrong),"native frame generation exact");
    wrong=selected;++wrong.surface.handle;require(!rig.receiver.selectedMetadata(wrong),"surface handle exact");
    wrong=selected;++wrong.surface.generation;require(!rig.receiver.selectedMetadata(wrong),"surface content generation exact");
    wrong=selected;++wrong.authority.streamGeneration;require(!rig.receiver.selectedMetadata(wrong),"native stream generation exact");
    const auto match=rig.receiver.selectedMetadata(selected);
    require(match&&match->sequence==17&&match->freshToken==nonce(97),"only complete exact tuple resolves observed metadata");
    require(!rig.sender.coveragePermitted(),"even exact metadata selection grants no mask/eye/mapping permission");
    ++decoded.native.key;++decoded.surface.generation;
    require(rig.receiver.observeDecoded(decoded)==rx::Result::Ordinary,"reused surface newer content association reported");
    require(!rig.receiver.selectedMetadata(selected),"old selected tuple invalid after actual surface reuse");
}
void codecGenerationAndLoss(){
    Rig rig;rig.exchange();auto changed=source();++changed.encoderGeneration;
    const auto ordinaryCopy=ordinary;const auto result=rig.sender.prepareProbe(bytes(ordinary),changed,19,nonce(100));
    require(!result.appended&&result.output.data==ordinary.data()&&result.output.size==ordinary.size()&&ordinary==ordinaryCopy,"codec generation change preserves actual original video");
    require(result.status==tx::SenderStatus::SourceChanged&&!rig.sender.requested()&&!rig.sender.mutualConfirmed()&&!rig.sender.coveragePermitted(),"stale codec identity revokes old bridge authority");
    auto stale=rig.context();++stale.authority.streamGeneration;
    require(rig.observe(ordinary,stale).result==rx::Result::WrongSession,"new codec stream not relabelled as old receiver");
    rig.producerAuth.reset();rig.receiverAuth.reset();rig.receiver.end();rig.sender.stop();
    require(!rig.receiver.snapshot().active&&!rig.producerAuth.bound()&&!rig.receiverAuth.bound(),"explicit transport loss resets all owners without timeout");
    const auto inactive=rig.receiver.observeAccessUnit(bytes(ordinary),rig.context());
    require(inactive.original.data==ordinary.data()&&inactive.original.size==ordinary.size()&&inactive.result==rx::Result::WrongSession,"post-loss actual video remains exact ordinary pass-through");
}
}
int main(int argc,char** argv){try{
    if(argc>2)throw std::runtime_error("usage: test_stream_bridge_authenticated [complete-au.hevc]");
    if(argc==2){std::ifstream file(argv[1],std::ios::binary);if(!file)throw std::runtime_error("cannot read complete AU");
        ordinary.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
        require(!ordinary.empty()&&ordinary.size()<=sb::MaximumAccessUnitBytes,"external complete AU bounded");}
    missingAndOrdinary();refusals();authenticatedProbeAndSelection();codecGenerationAndLoss();
    std::cout<<checks<<" authenticated sender/SEI/receiver integration checks passed ("<<(argc==2?"external supplied coded AU":"structural Annex-B fixture")<<")\n";
    return EXIT_SUCCESS;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}}
