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
    bool randomFails=false,routeReady=true;std::uint64_t clock=100;std::vector<nd::RuntimeNotice> notices;std::vector<nd::CaptureObservation> invalidations;
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
    nd::SourceProvider provider(){return {poll,pollCapture,stats,random,this,monotonic};}
    static std::uint64_t monotonic(void* v) noexcept{return static_cast<Fixture*>(v)->clock;}
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
    std::vector<std::uint8_t> certifiedFrame(){std::vector<std::uint8_t> coded;
        const sb::FrameDescriptor descriptor{++sourceFrame,81,4,4};const std::array<std::uint8_t,2> coverage{{16,255}};
        check(producer.certifyAccessUnit(bytes(ordinary),descriptor,sb::CoverageEncoding::Rle,bytes(coverage),coded),"actual paired steady frame certificate");return coded;}
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
void lateTransportReadiness(){
    Fixture f;const auto request=f.request();const auto port=f.producer.localPort();f.producer.stop();
    check(f.worker.prepare(request,f.provider()),"actual receiver prepared before the producer listener exists");
    f.enqueue(ordinary,9);f.pump();
    check(f.worker.status().state==nd::RuntimeState::SenderUnavailable&&f.randomCalls==1,
        "connection refusal revokes the initial source permission");
    const auto first=f.worker.status().source;
    f.clock+=249;f.enqueue(ordinary,10);f.pump();
    check(f.randomCalls==1,"unavailable listener never triggers a hot retry from repeated worker ticks or QBUFs");
    f.clock+=1;f.enqueue(ordinary,11);f.pump();
    check(f.randomCalls==2,"fresh successful native input retries once after the transport backoff");
    // An unavailable sender is not a permanent poison flag. Retrying remains
    // rate-bounded even when the user takes much longer than initial startup.
    for(unsigned i=0;i<10;++i){f.clock+=2000;f.enqueue(ordinary,20+i);f.pump();}
    check(f.randomCalls==12&&f.certified==0&&f.decoded==0,
        "late sender remains recoverable after many bounded attempts without admitting ordinary pixels");
    f.relisten(port);f.clock+=2000;f.enqueue(ordinary,60);f.pump();
    for(std::uint32_t token=61;token<64;++token){f.enqueue(f.certifiedDiscovery(),token);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,21,token});f.pump();}
    const auto fresh=f.worker.status().source;
    check(f.sender.confirmed()&&f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.certified==3&&f.decoded==3,
        "late real listener recovers through actual certified video and reverse authenticated control");
    check(fresh.outputPoolGeneration==first.outputPoolGeneration&&fresh.stream.generation>first.stream.generation
        &&fresh.stream.connection!=first.stream.connection,"same physical decoder pool gets a fresh cryptographic admission");
}
void transportRecoveryRejectsOldProof(){
    Fixture previous;previous.negotiate();const auto oldCertificate=previous.certifiedFrame();
    Fixture f;const auto request=f.request();const auto port=f.producer.localPort();f.producer.stop();
    check(f.worker.prepare(request,f.provider()),"source pending before late transport fixture prepared");
    f.enqueue(ordinary,40);f.pump();const auto old=f.worker.status().source;
    const auto oldCertified=f.certified,oldDecoded=f.decoded;
    check(f.worker.status().state==nd::RuntimeState::SenderUnavailable,"transport refusal withdraws all old pending proof before retry");
    f.relisten(port);f.clock+=250;f.enqueue(ordinary,41);f.pump();
    f.enqueue(oldCertificate,42);f.pump();
    check(f.certified==oldCertified,"old signed video cannot authenticate a fresh receiver admission");
    f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,42,7,21,42});f.pump();
    for(std::uint32_t token=50;token<53;++token){f.enqueue(f.certifiedDiscovery(),token);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,21,token});f.pump();}
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.worker.status().source.stream.connection!=old.stream.connection,
        "transport recovery requires another full native-video handshake");
    const auto beforeReuse=f.certified;
    f.enqueue(f.certifiedFrame(),40);f.pump();
    f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
    check(f.certified==beforeReuse&&f.decoded==oldDecoded+3,
        "retired pending native token cannot join new proof to a late old capture");
    f.enqueue(f.certifiedFrame(),54);f.pump();f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,54,7,21,54});f.pump();
    check(f.decoded==oldDecoded+4,"unrelated fresh exact frame still decodes after old completion is retired");
    Fixture fatal;auto r=fatal.request();r.pairingsDirectory=fatal.files.root/"missing";
    check(fatal.worker.prepare(r,fatal.provider()),"hard credential failure fixture prepared");fatal.enqueue(ordinary,71);fatal.pump();
    for(unsigned i=0;i<4;++i){fatal.clock+=10000;fatal.enqueue(ordinary,72+i);fatal.pump();}
    check(fatal.randomCalls==1&&fatal.worker.status().state==nd::RuntimeState::PairingRefused,
        "transport retry never retries a missing credential or weakens hard refusal");
    check(fatal.worker.status().control==sb::ControlStatus::PairingRefused,"failure notice retains the actual refusal after closing control");
    Fixture capacity;check(capacity.worker.prepare(capacity.request(),capacity.provider()),"hard capacity fixture prepared");
    capacity.enqueue(ordinary,80);capacity.input.front().fact.bufferCapacity=128;capacity.pump();
    for(unsigned i=0;i<4;++i){capacity.clock+=10000;capacity.enqueue(ordinary,81+i);capacity.pump();}
    check(capacity.randomCalls==1&&capacity.worker.status().control==sb::ControlStatus::CapacityRefused,
        "transport retry never retries insufficient native AU capacity");
    Fixture auth;put(auth.files.receiver/(std::string(id)+".json"),std::string("{\"schema\":\"sfnp-pairing-v1\",\"pair_id\":\"")+id
        +"\",\"key_hex\":\"0202030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\",\"role\":\"receiver\",\"protocol_major\":1}");
    check(auth.worker.prepare(auth.request(),auth.provider()),"mismatched private key fixture prepared");auth.enqueue(ordinary,90);auth.pump();
    for(unsigned i=0;i<4;++i){auth.clock+=10000;auth.enqueue(ordinary,91+i);auth.pump();}
    check(auth.randomCalls==1&&auth.worker.status().control==sb::ControlStatus::AuthenticationRefused&&auth.certified==0,
        "transport retry never retries authenticated-control rejection or grants pixels");
}
}
int main(){try{offAndPreparation();realVideoAndCapture();unprovedInputs();lifecycleAndLoss();ambiguityAndBound();imageRouteAdmission();lateTransportReadiness();transportRecoveryRejectsOldProof();std::cout<<checks<<" hosted native receiver checks PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
