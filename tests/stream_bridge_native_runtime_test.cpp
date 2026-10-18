#include "receiver/stream_bridge_native_runtime.h"
#include "receiver/stream_bridge_native_loader.h"
#include "pc/stream-bridge-sender.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace sb=stream_bridge;namespace nd=stream_bridge_native;namespace tx=stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool v,const char* name){++checks;if(!v)throw std::runtime_error(name);}
template<std::size_t N>sb::Bytes bytes(const std::array<std::uint8_t,N>& b){return {b.data(),b.size()};}
sb::Bytes bytes(const std::vector<std::uint8_t>& b){return {b.data(),b.size()};}
const std::vector<std::uint8_t> ordinary{0,0,0,1,0x26,1,0x80,0x11,0x22};
const char* id="0102030405060708090a0b0c0d0e0f10";
void put(const std::filesystem::path& path,const std::string& value){int fd=open(path.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW,0600);check(fd>=0,"private file fixture");check(write(fd,value.data(),value.size())==static_cast<ssize_t>(value.size()),"exact fixture write");close(fd);}
struct Files {
    std::filesystem::path root,producer,receiver;
    Files(){char name[]="/tmp/sfnp-runtime712-XXXXXX";check(mkdtemp(name)!=nullptr,"private runtime fixture");root=name;producer=root/"producer";receiver=root/"receiver";
        check(mkdir(producer.c_str(),0700)==0&&mkdir(receiver.c_str(),0700)==0,"private role fixture");
        for(const auto& role:{std::make_pair(producer,"producer"),std::make_pair(receiver,"receiver")}){
            put(role.first/"active.json",std::string("{\"schema\":\"sfnp-active-pairing-v1\",\"pair_id\":\"")+id+"\"}");
            put(role.first/(std::string(id)+".json"),std::string("{\"schema\":\"sfnp-pairing-v1\",\"pair_id\":\"")+id+"\",\"key_hex\":\"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\",\"role\":\""+role.second+"\",\"protocol_major\":1}");}}
    ~Files(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}
};
struct Event {nd::Observation fact{};std::vector<std::uint8_t> coded;};
struct Fixture {
    Files files;std::deque<Event> input;std::deque<nd::CaptureObservation> capture;
    nd::SnapshotStats counts{};unsigned polls=0,capturePolls=0,randomCalls=0,certified=0,decoded=0;
    bool randomFails=false,routeReady=true;std::vector<nd::RuntimeNotice> notices;std::vector<nd::CaptureObservation> invalidations;
    nd::DecodedFrame lastDecoded{};nd::CertifiedFrame lastCertified{};sb::CertifiedAccessUnit retained{};
    sb::ControlRuntime producer;nd::ReceiverWorker worker;sb::Handshake sender;
    sb::LocalConfig local{};sb::LocalSendAuthority authority{};std::uint64_t content=0,sourceFrame=1000;
    Fixture(){sb::ControlRequest r;r.explicitlyEnabled=true;r.role=sb::Role::Producer;r.buildId=712;r.pairingsDirectory=files.producer;
        r.offered=r.required=sb::KnownCapabilities;
        std::strcpy(r.address.data(),"127.0.0.1");check(producer.prepare(r)==sb::ControlStatus::Listening,"actual producer transport");
        check(producer.localConfig(local,authority),"actual producer signing session");
        check(sender.begin(local,authority.channel)==sb::Status::Ok,"actual full-capability producer handshake");}
    static tx::VerifiedSource source(){tx::VerifiedSource s;s.verified=s.ordinaryStream=s.completeAccessUnit=true;s.codec=tx::VerifiedSource::Codec::HevcAnnexB;s.moduleSha256[0]=1;s.apiVersion=0x7102000b;s.encoderGeneration=81;return s;}
    nd::RuntimeRequest request(){nd::RuntimeRequest r;r.explicitlyEnabled=true;r.buildId=712;r.pairingsDirectory=files.receiver;r.producerPort=producer.localPort();std::strcpy(r.producerAddress.data(),"127.0.0.1");
        r.imageRoutePrepared=[](void* v) noexcept{return static_cast<Fixture*>(v)->routeReady;};r.imageRouteContext=this;
        r.callbacks={onCertified,onDecoded,onCapture,onNotice,this};return r;}
    nd::SourceProvider provider(){return {poll,pollCapture,stats,random,this};}
    static bool poll(nd::Observation& out,std::uint8_t* destination,std::size_t capacity,void* v) noexcept {
        auto& f=*static_cast<Fixture*>(v);++f.polls;if(f.input.empty())return false;const auto& front=f.input.front();if(front.coded.size()>capacity)return false;
        out=front.fact;std::copy(front.coded.begin(),front.coded.end(),destination);f.input.pop_front();return true;}
    static bool pollCapture(nd::CaptureObservation& out,void* v) noexcept {auto& f=*static_cast<Fixture*>(v);++f.capturePolls;if(f.capture.empty())return false;out=f.capture.front();f.capture.pop_front();return true;}
    static nd::SnapshotStats stats(void* v) noexcept{return static_cast<Fixture*>(v)->counts;}
    static bool random(void* out,std::size_t n,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.randomCalls;if(f.randomFails)return false;std::memset(out,static_cast<int>(f.randomCalls),n);return true;}
    static void onCertified(const nd::CertifiedFrame& frame,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.certified;f.lastCertified=frame;f.retained=*frame.certificate;f.lastCertified.certificate=&f.retained;}
    static void onDecoded(const nd::DecodedFrame& frame,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.decoded;f.lastDecoded=frame;}
    static void onCapture(const nd::SourceIdentity&,const nd::CaptureObservation& c,void* v) noexcept{static_cast<Fixture*>(v)->invalidations.push_back(c);}
    static void onNotice(const nd::RuntimeNotice& n,void* v) noexcept{static_cast<Fixture*>(v)->notices.push_back(n);}
    void enqueue(std::vector<std::uint8_t> coded,std::uint32_t token,std::uint64_t pool=7){nd::Observation o{23,2,static_cast<std::uint32_t>(coded.size()),pool,++content,token,false,4096};input.push_back({o,std::move(coded)});}
    void pump(){for(unsigned i=0;i<80;++i){worker.tick();producer.tick();sb::VerifiedControl got{};while(producer.pollVerified(got))check(sender.receive(bytes(got.payload),got.channel)==sb::Status::Ok,"actual verified reverse handshake");}}
    std::vector<std::uint8_t> signedDiscovery(){sb::Message message{};check(sender.next(message)==sb::Status::Ok,"actual producer discovery action");
        sb::AuthPayload plain{};check(sb::encode(message,plain)==sb::Status::Ok,"actual producer full-capability encoding");
        std::vector<std::uint8_t> envelope,coded;const auto sealer=producer.senderSealer();
        check(sealer.seal(bytes(plain),envelope,sealer.context),"actual protected video envelope");
        check(sb::appendOwned(bytes(ordinary),bytes(envelope),coded)==sb::SeiStatus::Ok,"actual owned discovery SEI");
        check(sender.markSent(message,authority.channel)==sb::Status::Ok,"actual encoded source handoff receipt");return coded;}
    std::vector<std::uint8_t> certifiedDiscovery(){const auto discovery=signedDiscovery();
        std::vector<std::uint8_t> certified;const sb::FrameDescriptor descriptor{++sourceFrame,81,4,4};
        const std::array<std::uint8_t,2> coverage{{16,255}};
        check(producer.certifyAccessUnit(bytes(discovery),descriptor,sb::CoverageEncoding::Rle,bytes(coverage),certified),"actual paired exact AU certificate");
        return certified;}
    void start(){check(worker.prepare(request(),provider()),"actual hosted worker prepared");enqueue(ordinary,9);pump();
        capture.push_back({nd::CaptureKind::Decoded,23,73,0,9,7,21,9});pump();}
    void negotiate(){start();for(std::uint32_t token=10;token<13;++token){enqueue(certifiedDiscovery(),token);pump();
            capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,21,token});pump();}
        check(sender.confirmed()&&worker.status().state==nd::RuntimeState::PeerConfirmed,"actual video certificate plus real reverse socket confirms peer");}
    void relisten(std::uint16_t port){sb::ControlRequest r;r.explicitlyEnabled=true;r.role=sb::Role::Producer;r.buildId=712;r.pairingsDirectory=files.producer;
        r.offered=r.required=sb::KnownCapabilities;r.port=port;std::strcpy(r.address.data(),"127.0.0.1");
        check(producer.prepare(r)==sb::ControlStatus::Listening&&producer.localConfig(local,authority),"actual fresh producer control admission on same configured endpoint");
        sender.reset();check(sender.begin(local,authority.channel)==sb::Status::Ok,"new real paired binding after route reconnect");}
};
void offAndPreparation(){
    nd::ReceiverWorker worker;nd::RuntimeRequest r;r.pairingsDirectory="/does/not/exist";
    check(worker.prepare(r,{})&&worker.status().state==nd::RuntimeState::Disabled,"off path ignores missing credentials/provider");worker.tick();
    check(nd::startPinnedReceiver(r)&&!nd::pinnedReceiverRunning()&&!nd::decodeImportsPublished(),"default-off process entry never installs or starts");
    r.explicitlyEnabled=true;r.buildId=712;r.producerPort=7;check(!worker.prepare(r,{}),"incomplete actual source ports refused");
}
void realVideoAndCapture(){
    Fixture f;f.negotiate();check(f.certified==3&&f.decoded==3,"every exact certified QBUF joins actual capture once");
    check(f.lastDecoded.descriptor.sourceFrame==1003&&f.lastDecoded.capture.dmabufFd==73
        &&f.lastDecoded.capture.capturePoolGeneration==21&&f.lastDecoded.submitted.nativeTimestampToken==12
        &&f.lastDecoded.submitted.contentGeneration==4,"source frame, native token, output content and capture leases stay distinct");
    check(f.lastCertified.source.stream.connection[0]==1&&f.lastCertified.source.stream.generation==7
        &&!f.lastCertified.submitted.completeAccessUnit&&f.retained.coverageEncoding==sb::CoverageEncoding::Rle,"authenticated completeness never edits original native fact");
    check(f.lastCertified.peerConfirmed||f.lastDecoded.peerConfirmed,"full mutual state reported separately from image proof");
    check(f.retained.original==ordinary||f.retained.original.size()>ordinary.size(),"owned certificate data is synchronously copyable");
    const auto old=f.decoded;f.capture.push_back(f.lastDecoded.capture);f.pump();check(f.decoded==old,"duplicate decoded token cannot replay consumed certificate");
    f.capture.push_back({nd::CaptureKind::Requeue,23,73,0,0,7,21,99});f.pump();check(!f.invalidations.empty()&&f.invalidations.back().kind==nd::CaptureKind::Requeue,"surface reuse forwards exact invalidation before any new decode");
    f.worker.stop();check(f.worker.status().state==nd::RuntimeState::Disabled,"explicit stop revokes worker state");
}
void unprovedInputs(){
    Fixture f;f.start();
    // An actual authenticated HELLO without the exact original-AU certificate
    // must not bootstrap, even though it is valid and fully contained.
    const auto d=f.signedDiscovery();
    f.enqueue(d,20);f.pump();
    check(f.certified==0&&f.decoded==0&&f.worker.status().state!=nd::RuntimeState::PeerConfirmed,"signed HELLO alone never fabricates complete encoder AU");
    Fixture bad;bad.start();auto corrupt=bad.certifiedDiscovery();corrupt.back()^=1;bad.enqueue(corrupt,21);bad.pump();
    check(bad.certified==0,"actual original bytes mismatch refuses certificate before consumer");
    bad.capture.push_back({nd::CaptureKind::Decoded,23,73,0,21,7,21,1});bad.pump();check(bad.decoded==0,"uncertified ordinary output cannot become a selected frame");
    f.enqueue(ordinary,22);f.pump();check(f.certified==0,"ordinary unpatched Steam Link remains unobserved");
}
void lifecycleAndLoss(){
    Fixture f;f.negotiate();const auto before=f.certified;
    f.capture.push_back({nd::CaptureKind::Reset,23,-1,0,0,7,22,0});f.pump();
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed,"capture-only reallocation does not fabricate native stream reconnect");
    f.capture.push_back({nd::CaptureKind::SourceReset,23,-1,0,0,7,23,0});f.pump();
    check(f.worker.status().state==nd::RuntimeState::SourceLost,"actual OUTPUT retirement revokes source immediately");
    f.enqueue(ordinary,90);f.pump();check(f.certified==before&&f.randomCalls==1,"same revoked pool cannot resurrect trust or spin retries");
    f.enqueue(ordinary,91,8);f.pump();check(f.randomCalls==2&&f.worker.status().source.stream.generation==8
        &&f.worker.status().source.stream.connection[0]==2,"only a new actual source pool creates a fresh receiving identity");
    Fixture loss;loss.negotiate();++loss.counts.dropped;loss.pump();check(loss.worker.status().state==nd::RuntimeState::SourceLost,"actual coded snapshot loss refuses rather than newest-frame fallback");
    Fixture events;events.negotiate();++events.counts.captureDropped;events.pump();check(events.worker.status().state==nd::RuntimeState::SourceLost,"capture event loss invalidates all evidence");
    Fixture rejected;rejected.negotiate();++rejected.counts.rejected;rejected.pump();check(rejected.worker.status().state==nd::RuntimeState::SourceLost,"failed coded ownership copy invalidates pending token association");
    Fixture eof;eof.negotiate();eof.producer.stop();eof.pump();check(eof.worker.status().state==nd::RuntimeState::SenderUnavailable,"actual reverse transport EOF revokes confirmed session");
}
void ambiguityAndBound(){
    Fixture f;f.start();f.enqueue(f.certifiedDiscovery(),30);f.pump();check(f.certified==1,"first unique token admitted");
    f.enqueue(ordinary,30);f.pump();check(f.worker.status().state==nd::RuntimeState::SourceLost,"same live native token never aliases a newer source frame");
    Fixture full;full.start();for(unsigned i=0;i<nd::ReceiverWorker::PendingCapacity+1;++i){full.enqueue(ordinary,100+i);full.pump();}
    check(full.worker.status().state==nd::RuntimeState::SourceLost,"in-flight table capacity refuses without eviction");
    Fixture random;random.randomFails=true;random.start();random.enqueue(ordinary,70);random.pump();
    check(random.worker.status().state==nd::RuntimeState::SourceRefused&&random.certified==0,"failed source-session OS randomness never uses weak ID fallback");
    Fixture pairing;auto r=pairing.request();r.pairingsDirectory=pairing.files.root/"missing";check(pairing.worker.prepare(r,pairing.provider()),"missing credentials checked on worker after real source");
    pairing.enqueue(ordinary,71);pairing.pump();check(pairing.worker.status().state==nd::RuntimeState::PairingRefused,"missing selected receiver credential gets bounded status");
}
void imageRouteAdmission(){
    Fixture waiting;waiting.routeReady=false;waiting.start();
    check(waiting.worker.status().state==nd::RuntimeState::WaitingImageRoute&&waiting.randomCalls==0&&waiting.polls==0,
        "unprepared final image route starts no source identity, full-cap control or repeating source copies");
    waiting.routeReady=true;waiting.pump();check(waiting.randomCalls==0&&waiting.input.empty(),"first route readiness discards held stale snapshots and waits for new native input");
    waiting.enqueue(ordinary,30);waiting.pump();check(waiting.randomCalls==1&&waiting.worker.status().state==nd::RuntimeState::Negotiating,"new actual QBUF alone begins ready-route capacity/auth admission");
    Fixture missing;auto r=missing.request();r.imageRoutePrepared=nullptr;check(missing.worker.prepare(r,missing.provider()),"missing route proof stays configured ordinary");
    missing.enqueue(ordinary,31);missing.pump();check(missing.randomCalls==0&&missing.worker.status().state==nd::RuntimeState::WaitingImageRoute,"missing route callback defaults false");
    Fixture f;f.negotiate();const auto original=f.worker.status().source;const auto port=f.producer.localPort();
    f.routeReady=false;f.pump();check(f.worker.status().state==nd::RuntimeState::WaitingImageRoute&&f.producer.status()==sb::ControlStatus::Lost,
        "actual image-route loss closes existing authenticated control and revokes prior admission");
    f.relisten(port);f.routeReady=true;f.pump();f.enqueue(ordinary,40);f.pump();
    for(std::uint32_t token=41;token<44;++token){f.enqueue(f.certifiedDiscovery(),token);f.pump();f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,21,token});f.pump();}
    const auto fresh=f.worker.status().source;
    check(f.sender.confirmed()&&f.worker.status().state==nd::RuntimeState::PeerConfirmed&&fresh.outputPoolGeneration==original.outputPoolGeneration
        &&fresh.stream.generation>original.stream.generation&&fresh.stream.connection!=original.stream.connection,
        "same actual native pool recovers only through fresh route admission lifecycle and full real handshake");
}
}
int main(){try{offAndPreparation();realVideoAndCapture();unprovedInputs();lifecycleAndLoss();ambiguityAndBound();imageRouteAdmission();std::cout<<checks<<" hosted native receiver checks PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
