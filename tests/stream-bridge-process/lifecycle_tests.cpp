#include <cstdio>
#include <stdexcept>
#include <future>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../stream-bridge-camera-client/helper_fixture.h"
#ifndef SFNP_PROCESS_SOURCE
#define SFNP_PROCESS_SOURCE "../../layer/stream_bridge_process.cpp"
#endif
#include SFNP_PROCESS_SOURCE
namespace renderer=stream_bridge_renderer;
namespace {
unsigned checks=0;
void check(bool valid,const char* message){++checks;if(!valid)throw std::runtime_error(message);}
struct Directory {
    std::filesystem::path path;
    Directory(){char name[]="/tmp/sfnp-process712-XXXXXX";check(mkdtemp(name)!=nullptr,"private fixture directory created");path=name;}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
struct File {
    int fd=-1;File(){fd=memfd_create("process-fixture",MFD_CLOEXEC);check(fd>=0&&ftruncate(fd,4096)==0,"actual owned image allocation");}
    ~File(){if(fd>=0)close(fd);}
};
local::PeerPin selfPin(){
    FILE* file=fopen("/proc/self/exe","rb");check(file!=nullptr,"read exact fixture peer executable");
    frame_receiver::Sha256 sha;std::array<unsigned char,16384> bytes{};std::size_t total=0,n=0;
    while((n=fread(bytes.data(),1,bytes.size(),file))){sha.update(bytes.data(),n);total+=n;}
    fclose(file);return {total,sha.finish()};
}
void connect(renderer::Process& p,local::Channel& producer,const Directory& directory){
    const auto pin=selfPin();p.kind=renderer::ProcessKind::Compositor;p.root=directory.path;
    p.connection=std::make_shared<std::atomic<bool>>(true);
    const auto path=p.root/"receiver.sock";
    check(p.channel.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"real checked consumer socket prepared");
    check(producer.preparePinned(local::Endpoint::VrlinkProducer,path,pin),"real checked producer socket prepared");
    for(unsigned n=0;n<80;++n){p.channel.tick();producer.tick();}
    check(p.channel.state()==local::State::Connected&&producer.state()==local::State::Connected,"both actual channels authenticated by SO_PEERCRED and executable pin");
}
void noConsumers(){
    Directory directory;renderer::Process p;local::Channel producer;connect(p,producer,directory);
    const auto connection=p.connection;local::State previous=local::State::Connected;
    check(renderer::imageRoutePrepared(&p),"connected route is initially visible to decoder worker");
    renderer::workCycle(p,previous);
    check(!connection->load(std::memory_order_acquire),"last consumer disappearance immediately revokes old picture connection");
    check(p.channel.state()==local::State::Off&&previous==local::State::Off,"actual worker cycle closes channel when no consumer exists");
    check(!renderer::imageRoutePrepared(&p),"missing consumer withdraws producer route admission");
    check(!std::filesystem::exists(directory.path/"receiver.sock"),"worker removes only its owned consumer socket");
    local::Message message;message.source.stream.connection.fill(1);message.source.stream.generation=message.source.outputPoolGeneration=1;
    message.descriptor={1,1,1,1};message.binding.fill(2);message.submittedContent=1;message.peerConfirmed=true;message.certifiedMetadata={1};
    check(producer.enqueue(message),"producer attempts its next real metadata send after peer teardown");
    producer.tick();check(producer.state()==local::State::Lost,"actual next producer send detects the closed route");
}
struct Consumer {bool ready=false;unsigned ticks=0;};
bool admitted(void* pointer) noexcept{return static_cast<Consumer*>(pointer)->ready;}
void tick(void* pointer) noexcept{++static_cast<Consumer*>(pointer)->ticks;}
void consumerAdmission(){
    Directory directory;renderer::Process p;local::Channel producer;connect(p,producer,directory);
    Consumer consumer;p.consumers[0]={&consumer,nullptr,nullptr,tick,admitted};local::State previous=local::State::Connected;
    auto old=p.connection;renderer::workCycle(p,previous);
    check(p.channel.state()==local::State::Off&&!old->load(std::memory_order_acquire)&&consumer.ticks==0,"present but unadmitted consumer cannot keep image route active");
    producer.stop();connect(p,producer,directory);old=p.connection;consumer.ready=true;previous=local::State::Connected;
    renderer::workCycle(p,previous);
    check(p.channel.state()==local::State::Connected&&old->load(std::memory_order_acquire),"admitted real consumer retains native connected transport");
    check(consumer.ticks==1&&renderer::imageRoutePrepared(&p),"only admitted consumer receives worker tick and grants producer route");
    consumer.ready=false;renderer::workCycle(p,previous);
    check(!old->load(std::memory_order_acquire)&&p.channel.state()==local::State::Off&&consumer.ticks==1,"revoked consumer cannot retain connection or receive another callback");
}
void sourceLossReconnect(){
    renderer::Process p;File file;check(p.writer.prepareCold()&&p.writer.associateResourceFd(file.fd),"process writer prepared with actual resource identity");
    native::CaptureObservation capture{native::CaptureKind::Decoded,23,file.fd,2,71,7,21,1};
    p.writer.captureEvent(capture);local::LeaseReference old;
    check(p.writer.reference(capture,old),"actual source capture has first-epoch reference");
    const int fd=p.writer.duplicateReadOnly();local::LeaseReader before;
    check(fd>=0&&before.mapReadOnly(fd)&&before.current(old),"existing consumer sees current exact source lease");
    native::RuntimeNotice notice;notice.state=native::RuntimeState::Negotiating;renderer::sourceNotice(notice,&p);
    check(before.current(old),"unrelated negotiating notice does not revoke a valid source lease");
    notice.state=native::RuntimeState::SenderUnavailable;renderer::sourceNotice(notice,&p);
    check(!before.current(old),"real SenderUnavailable callback invalidates old source reader immediately");
    ++capture.contentGeneration;p.writer.captureEvent(capture);local::LeaseReference fresh;
    check(p.writer.reference(capture,fresh)&&fresh.epoch>old.epoch,"source callback uses nonterminal epoch and permits actual new capture");
    local::LeaseReader after;check(after.mapReadOnly(fd)&&after.current(fresh)&&!after.current(old)&&!before.current(fresh),"fresh reconnect reader succeeds without reviving any old reference or mapping");
    check(after.epochCurrent(fresh),"source epoch is independently current before ordinary native requeue");
    auto requeue=capture;requeue.kind=native::CaptureKind::Requeue;p.writer.captureEvent(requeue);
    check(!after.current(fresh)&&after.epochCurrent(fresh),"ordinary native buffer requeue retires picture authority without falsely retiring source session");
    ++capture.contentGeneration;p.writer.captureEvent(capture);check(p.writer.reference(capture,fresh)&&after.current(fresh),"same source epoch permits actual newly decoded content after requeue");
    notice.state=native::RuntimeState::SourceLost;renderer::sourceNotice(notice,&p);
    check(!after.current(fresh),"real SourceLost callback also invalidates current source epoch");
    check(!after.epochCurrent(fresh),"source restart revokes independent epoch readiness even when buffer identity is unchanged");
    local::LeaseReference next;check(p.writer.reference(capture,next)&&next.epoch>fresh.epoch,"successive source losses still allow a future admission epoch");
    close(fd);
}
void telemetry(){
    renderer::Process p;p.kind=renderer::ProcessKind::Compositor;
    native::CertifiedFrame callback;renderer::certified(callback,&p);
    check(p.flow[unsigned(renderer::Flow::CertifiedCallback)]==1&&p.flow[unsigned(renderer::Flow::CertifiedQueued)]==0,
        "an unconfirmed certificate callback never counts as locally queued metadata");
    native::DecodedFrame decoded;renderer::decoded(decoded,&p);
    check(p.flow[unsigned(renderer::Flow::DecodedCallback)]==1&&p.flow[unsigned(renderer::Flow::DecodedQueued)]==0,
        "an unconfirmed decoded callback never counts as a queued picture");
    local::Message invalid;invalid.peerConfirmed=true;invalid.kind=local::Kind::Certified;
    renderer::consume(p,invalid);
    check(p.flow[unsigned(renderer::Flow::CertifiedReceived)]==1&&p.flow[unsigned(renderer::Flow::AuthenticationRefused)]==1&&p.flow[unsigned(renderer::Flow::Authenticated)]==0,
        "invalid detached metadata is separately counted without image authority");
    invalid.kind=local::Kind::Decoded;renderer::consume(p,invalid);
    check(p.flow[unsigned(renderer::Flow::DecodedReceived)]==1&&p.flow[unsigned(renderer::Flow::CertificateMiss)]==1&&p.flow[unsigned(renderer::Flow::PicturesRetained)]==0,
        "decoded packet without exact certificate never counts as a retained local picture");
    renderer::reportFlowCold(p);const auto first=p.flowLines;
    for(unsigned i=0;i<4096;++i)renderer::reportFlowCold(p);
    check(first==1&&p.flowLines==first,"unchanged local flow never repeats cold status");
    for(unsigned bit=1;bit<64;++bit){p.flow[unsigned(renderer::Flow::CertifiedCallback)]=std::uint64_t(1)<<bit;renderer::reportFlowCold(p);}
    check(p.flowLines==48,"changed local status has an absolute per-process line bound");
    renderer::recordHandoffDuration(p,11000);renderer::recordHandoffDuration(p,22000);renderer::recordHandoffDuration(p,50000);renderer::recordHandoffDuration(p,51000);renderer::recordHandoffDuration(p,2);
    check(p.handoffMaxMicros==51000&&p.flow[unsigned(renderer::Flow::HandoffOverFrame)]==3&&p.flow[unsigned(renderer::Flow::HandoffOverTwoFrames)]==2&&p.flow[unsigned(renderer::Flow::HandoffOver50ms)]==1,"bounded handoff diagnostics retain high-water latency and strict threshold counts without changing readiness");
}
// Exercise the production Process and its actual authenticated local sockets.
// Only the final consumer's owned-storage publication is a controllable port;
// native GPU ordering is independently checked by the Renderer/Pool fixture.
struct Publication {
    bool ready=false;unsigned certificates=0,pictures=0,queries=0;
    static bool admitted(void*) noexcept{return true;}
    static void coverage(void* p,const sb::CertifiedAccessUnit&) noexcept{++static_cast<Publication*>(p)->certificates;}
    static void picture(void* p,const renderer::LocalPicture&) noexcept{++static_cast<Publication*>(p)->pictures;}
    static bool published(void* p,const renderer::LocalPicture&) noexcept{auto& v=*static_cast<Publication*>(p);++v.queries;return v.ready;}
};
struct HandoffFixture {
    Directory directory;File file,secondFile;renderer::Process source,display;Publication publication;
    local::State sourceState=local::State::Connected,displayState=local::State::Connected;
    sb::Pairing pair{};sb::Nonce binding{},connection{};sb::AuthSession sender,observer;std::uint64_t nextFrame=1000;
    HandoffFixture(bool warmCamera=true,bool delayedCamera=false){
        sfnp_camera_fixture::write(directory.path,delayedCamera?"gate":"normal");
        pair.role=sb::Role::Producer;pair.pairId.fill(11);pair.key.fill(101);binding.fill(41);connection.fill(71);
        check(sender.prepareProducer(pair,binding)==sb::AuthStatus::Ok,"actual handoff producer signer prepared");
        pair.role=sb::Role::Receiver;
        check(observer.prepareReceiver(pair,{connection,7})==sb::AuthStatus::Ok&&display.verifier.prepareReceiver(pair,{connection,7})==sb::AuthStatus::Ok,"actual independent AU and detached metadata verifiers prepared");
        source.kind=renderer::ProcessKind::Vrlink;display.kind=renderer::ProcessKind::Compositor;
        source.root=display.root=directory.path;source.connection=std::make_shared<std::atomic<bool>>(true);display.connection=std::make_shared<std::atomic<bool>>(true);
        source.peerConfirmed=true;display.verifiedSource={connection,7};
        display.consumers[0]={&publication,Publication::coverage,Publication::picture,nullptr,Publication::admitted,Publication::published};
        const auto pin=selfPin();const auto path=directory.path/"receiver.sock";
        check(display.channel.preparePinned(local::Endpoint::CompositorConsumer,path,pin)&&source.channel.preparePinned(local::Endpoint::VrlinkProducer,path,pin),"actual two Process channels prepared");
        for(unsigned n=0;n<16;++n){display.channel.tick();source.channel.tick();}
        check(source.channel.state()==local::State::Connected&&display.channel.state()==local::State::Connected,"handoff channel peers authenticated by kernel and exact executable");
        check(source.writer.prepareCold()&&source.writer.associateResourceFd(file.fd)&&source.writer.associateResourceFd(secondFile.fd),"actual shared capture writer owns both physical resources");
        if(warmCamera){
            display.camera.request(true);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!display.camera.ready()&&std::chrono::steady_clock::now()<end){display.camera.tickCold(directory.path,true);std::this_thread::yield();}
            check(display.camera.ready(),"independent actual helper acknowledges settings before GPU-only handoff fixture begins");
        }
    }
    ~HandoffFixture(){
        display.camera.stopCold();const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(display.camera.runningCold()&&std::chrono::steady_clock::now()<end){display.camera.tickCold(directory.path,false);std::this_thread::yield();}
    }
    void sendDecoded(local::Message decoded){
        decoded.dmabuf=decoded.lease.capture.dmabufFd;decoded.readOnlyLeases=source.writer.duplicateReadOnly();
        check(decoded.readOnlyLeases>=0&&source.channel.enqueue(decoded),"exact decoded capture queued separately after its certificate");close(decoded.readOnlyLeases);source.channel.tick();
    }
    local::Message queue(bool active=true,unsigned index=0,bool includeDecoded=true){
        const int image=index?secondFile.fd:file.fd;
        const auto frame=nextFrame++;
        native::CaptureObservation capture{native::CaptureKind::Decoded,23,image,2+index,71+index,7,21,frame};
        source.writer.captureEvent(capture);local::LeaseReference lease;
        check(source.writer.reference(capture,lease),"decoded source has exact current kernel lease before DQ handoff");
        const std::vector<std::uint8_t> nativeVideo{0,0,0,1,0x40,1,0x91,0x80,0,0,1,0x02,1,0x80,0x42,0x80,0,0};
        std::vector<std::uint8_t> raw(active?32:0,127),coded;const sb::FrameDescriptor descriptor{frame,41,16,2};
        check(sb::appendCertifiedFrame({nativeVideo.data(),nativeVideo.size()},descriptor,active?sb::CoverageEncoding::Raw:sb::CoverageEncoding::None,{raw.data(),raw.size()},sender,coded)==sb::FrameStatus::Ok,"owned native AU receives actual authenticated coverage certificate");
        sb::CertifiedAccessUnit certificate;check(sb::inspectCertifiedFrame({coded.data(),coded.size()},observer,certificate)==sb::FrameStatus::Ok&&certificate.original==nativeVideo,"complete original AU verified before detached local metadata publication");
        local::Message certified;certified.kind=local::Kind::Certified;certified.source.stream={connection,7};certified.source.outputPoolGeneration=7;certified.descriptor=descriptor;certified.binding=binding;certified.nativeTimestampToken=capture.nativeTimestampToken;certified.submittedContent=6;certified.peerConfirmed=true;
        check(sb::serializeCoverageMetadata(certificate,certified.certifiedMetadata),"real verified AU exports exact signed detached metadata");
        auto decoded=certified;decoded.kind=local::Kind::Decoded;decoded.certifiedMetadata.clear();decoded.lease=lease;decoded.dmabuf=image;decoded.readOnlyLeases=source.writer.duplicateReadOnly();
        check(decoded.readOnlyLeases>=0&&source.channel.enqueue(certified)&&(!includeDecoded||source.channel.enqueue(decoded)),"certificate and optional exact decoded capture queued on real socket");close(decoded.readOnlyLeases);
        decoded.dmabuf=decoded.readOnlyLeases=-1;source.awaiting[lease.slot]=decoded;source.acknowledged[lease.slot]={};source.channel.tick();return decoded;
    }
    void cycle(){renderer::workCycle(display,displayState);renderer::workCycle(source,sourceState);}
    std::future<void> start(const local::Message& decoded){return std::async(std::launch::async,[this,capture=decoded.lease.capture]{renderer::handoff(capture,&source);});}
    void waiting(){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(1);while(!source.flow[unsigned(renderer::Flow::HandoffWait)].load()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();check(source.flow[unsigned(renderer::Flow::HandoffWait)]==1,"exact native DQ handoff reaches asynchronous publication wait");}
    void receipt(local::Message message){message.kind=local::Kind::Ready;message.certifiedMetadata.clear();check(display.channel.enqueue(message),"receipt travels through actual authenticated reverse local channel");display.channel.tick();renderer::workCycle(source,sourceState);}
};
struct TransientPublication {
    unsigned decoded=0,legacy=0;std::uintptr_t address=0;std::vector<std::uint8_t> pixels;sb::CoverageRange range;std::uint64_t frame=0;
    static bool admitted(void*) noexcept{return true;}
    static void legacyCallback(void* pointer,const sb::CertifiedAccessUnit&) noexcept{++static_cast<TransientPublication*>(pointer)->legacy;}
    static void decodedCallback(void* pointer,const sb::CertifiedAccessUnit& certificate,sb::Bytes bytes,sb::CoverageRange range) noexcept{
        auto& p=*static_cast<TransientPublication*>(pointer);++p.decoded;p.address=reinterpret_cast<std::uintptr_t>(bytes.data);p.pixels.assign(bytes.data,bytes.data+bytes.size);p.range=range;p.frame=certificate.descriptor.sourceFrame;
    }
};
void decodedDispatch(){
    HandoffFixture f(false);TransientPublication first,second;Publication fallback;
    f.display.consumers[0]={&first,TransientPublication::legacyCallback,nullptr,nullptr,TransientPublication::admitted,nullptr,TransientPublication::decodedCallback};
    f.display.consumers[1]={&second,TransientPublication::legacyCallback,nullptr,nullptr,TransientPublication::admitted,nullptr,TransientPublication::decodedCallback};
    f.display.consumers[2]={&fallback,Publication::coverage,nullptr,nullptr,Publication::admitted};
    const auto queued=f.queue(true,0,false);f.cycle();
    check(first.decoded==1&&second.decoded==1&&!first.legacy&&!second.legacy&&fallback.certificates==1,"authenticated compositor expands once and dispatches decoded consumers without legacy redecoding while preserving legacy callbacks");
    check(first.address&&first.address==second.address&&first.pixels==std::vector<std::uint8_t>(32,127)&&second.pixels==first.pixels&&first.frame==queued.descriptor.sourceFrame&&first.range.minimum==127&&first.range.maximum==127,"all synchronous consumers borrow the same transient exact authenticated pixel storage and range");
    local::Message invalid;invalid.kind=local::Kind::Certified;invalid.peerConfirmed=true;invalid.source.stream=f.display.verifiedSource;invalid.certifiedMetadata={1};
    renderer::consume(f.display,invalid);
    check(first.decoded==1&&second.decoded==1&&fallback.certificates==1&&f.display.flow[unsigned(renderer::Flow::AuthenticationRefused)]==1,"malformed detached metadata never reaches either decoded or legacy consumer");
}
void exactHandoff(){
    HandoffFixture f;const auto decoded=f.queue();auto dq=f.start(decoded);f.waiting();
    f.cycle();check(f.publication.certificates==1&&f.source.flow[unsigned(renderer::Flow::ReadyReceived)]==0&&f.display.flow[unsigned(renderer::Flow::ReadyQueued)]==0,"active certificate and picture do not acknowledge unpublished exact mask storage");
    check(dq.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout,"native DQ result stays hidden until same exact local mask publication");
    auto bad=decoded;bad.binding[0]^=1;f.receipt(bad);
    bad=decoded;++bad.source.stream.generation;f.receipt(bad);
    bad=decoded;++bad.descriptor.sourceFrame;f.receipt(bad);
    bad=decoded;++bad.nativeTimestampToken;++bad.lease.capture.nativeTimestampToken;f.receipt(bad);
    bad=decoded;++bad.submittedContent;f.receipt(bad);
    bad=decoded;++bad.lease.epoch;f.receipt(bad);
    bad=decoded;bad.lease.sequence+=2;f.receipt(bad);
    bad=decoded;++bad.lease.capture.capturePoolGeneration;f.receipt(bad);
    bad=decoded;++bad.lease.capture.sourcePoolGeneration;++bad.source.outputPoolGeneration;f.receipt(bad);
    bad=decoded;++bad.lease.capture.contentGeneration;f.receipt(bad);
    bad=decoded;++bad.lease.kernel[1];f.receipt(bad);
    check(f.source.flow[unsigned(renderer::Flow::ReadyReceived)]==0&&dq.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout,"foreign binding/source/frame/capture/kernel and stale epoch receipts cannot expose native DQ");
    f.publication.ready=true;f.cycle();check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"matching publication returns through real reverse IPC and releases same DQ");dq.get();
    check(f.source.flow[unsigned(renderer::Flow::ReadyReceived)]==1&&f.source.flow[unsigned(renderer::Flow::HandoffReady)]==1&&f.source.flow[unsigned(renderer::Flow::HandoffTimeout)]==0,"only exact acknowledged source is released without emergency fallback");
}
void noneAndBurst(){
    {HandoffFixture f;f.publication.ready=true;const auto a=f.queue(),b=f.queue(true,1);renderer::workCycle(f.display,f.displayState);
     check(f.publication.certificates==2&&f.display.flow[unsigned(renderer::Flow::DecodedReceived)]==2&&f.display.flow[unsigned(renderer::Flow::ReadyQueued)]==2,"one actual worker cycle drains certificate/capture/certificate/capture burst without artificial 2-ms serialization");
     renderer::workCycle(f.source,f.sourceState);check(f.source.flow[unsigned(renderer::Flow::ReadyReceived)]==2,"same reverse worker cycle accepts both exact acknowledgments");
     auto first=f.start(a),second=f.start(b);first.get();second.get();check(f.source.flow[unsigned(renderer::Flow::HandoffReady)]==2&&!f.source.flow[unsigned(renderer::Flow::HandoffWait)],"already-published independent capture leases need no asynchronous wait");}
    {HandoffFixture f;const auto decoded=f.queue(false);auto dq=f.start(decoded);f.waiting();f.cycle();check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"authenticated None releases DQ immediately after local certificate and capture join");dq.get();check(!f.publication.queries&&f.display.flow[unsigned(renderer::Flow::ReadyQueued)]==1&&f.source.flow[unsigned(renderer::Flow::HandoffReady)]==1,"signed None never asks a consumer to allocate or complete GPU mask storage");}
}
void interruptedHandoff(){
    {HandoffFixture f;const auto decoded=f.queue();f.source.peerConfirmed=false;auto dq=f.start(decoded);check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"unpaired native video bypasses optional handoff wait");dq.get();check(!f.source.flow[unsigned(renderer::Flow::HandoffWait)],"unpaired flow never reports a mask publication wait");}
    {HandoffFixture f;const auto decoded=f.queue();auto dq=f.start(decoded);f.waiting();native::RuntimeNotice lost;lost.state=native::RuntimeState::SourceLost;renderer::sourceNotice(lost,&f.source);check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"source revoke wakes native handoff without a compositor-thread wait");dq.get();check(!f.source.flow[unsigned(renderer::Flow::HandoffReady)]&&!f.source.flow[unsigned(renderer::Flow::HandoffTimeout)],"revocation never fabricates successful mask readiness or emergency timeout");}
    {HandoffFixture f;const auto decoded=f.queue();auto dq=f.start(decoded);f.waiting();{std::lock_guard<std::mutex> lock(f.source.gate);f.source.channel.stop();f.source.released.notify_all();}check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"transport stop releases blocked native handoff");dq.get();check(!f.source.flow[unsigned(renderer::Flow::HandoffReady)],"dropped local channel never counts as exact readiness");}
    {HandoffFixture f;const auto decoded=f.queue();auto dq=f.start(decoded);f.waiting();auto requeue=decoded.lease.capture;requeue.kind=native::CaptureKind::Requeue;f.source.writer.captureEvent(requeue);f.source.released.notify_all();check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"native requeue invalidates and releases old exact DQ wait");dq.get();f.receipt(decoded);check(!f.source.flow[unsigned(renderer::Flow::ReadyReceived)]&&!f.source.flow[unsigned(renderer::Flow::HandoffReady)],"receipt for requeued capture cannot revive old mask authority");}
    {HandoffFixture f;const auto decoded=f.queue();const auto start=std::chrono::steady_clock::now();auto dq=f.start(decoded);dq.get();const auto elapsed=std::chrono::steady_clock::now()-start;check(elapsed>=std::chrono::milliseconds(80)&&elapsed<std::chrono::seconds(2),"genuine missing peer receipt uses bounded emergency deadline, never an indefinite native freeze");check(f.source.flow[unsigned(renderer::Flow::HandoffTimeout)]==1&&!f.source.flow[unsigned(renderer::Flow::HandoffReady)],"emergency release is explicitly counted and never misreported as an exact mask");}
}
void cameraHandoff(){
    HandoffFixture f(false,true);f.publication.ready=true;const auto decoded=f.queue();
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(sfnp_camera_fixture::events(f.directory.path).find("enable-pending")==std::string::npos&&std::chrono::steady_clock::now()<end){f.cycle();std::this_thread::yield();}
    check(sfnp_camera_fixture::events(f.directory.path).find("enable-pending")!=std::string::npos,"actual authenticated active metadata reaches isolated settings helper");
    check(!f.display.camera.ready()&&!f.display.flow[unsigned(renderer::Flow::ReadyQueued)],"published exact GPU mask cannot expose native DQ before camera API acknowledgment");
    auto dq=f.start(decoded);f.waiting();std::ofstream(f.directory.path/"enable-release")<<"acknowledged";
    const auto readyEnd=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!f.source.flow[unsigned(renderer::Flow::ReadyReceived)]&&std::chrono::steady_clock::now()<readyEnd){f.cycle();std::this_thread::yield();}
    check(f.display.camera.ready()&&f.source.flow[unsigned(renderer::Flow::ReadyReceived)]==1,"matching settings acknowledgment and exact mask jointly publish actual reverse Ready");
    check(dq.wait_for(std::chrono::seconds(1))==std::future_status::ready,"settings-gated native handoff finishes without compositor I/O");dq.get();
    auto requeue=decoded.lease.capture;requeue.kind=native::CaptureKind::Requeue;f.source.writer.captureEvent(requeue);f.cycle();
    check(f.display.camera.ready(),"ordinary requeue does not disable acknowledged global camera session");
    f.source.writer.invalidateCurrentEpoch();f.cycle();
    check(!f.display.camera.ready(),"shared native source epoch loss atomically revokes cached settings readiness before restore");
}
void cameraPictureDemand(){
    HandoffFixture f;f.publication.ready=true;const auto active=f.queue();f.cycle();
    check(f.display.camera.ready()&&f.display.cameraWanted,"exact active decoded picture retains acknowledged camera demand");
    const auto opaque=f.queue(false,1,false);f.cycle();
    check(f.display.camera.ready()&&f.display.cameraWanted&&!f.display.flow[unsigned(renderer::Flow::CameraNone)],"future signed None certificate cannot revoke camera from the current exact decoded active picture");
    f.sendDecoded(opaque);f.cycle();
    check(f.display.camera.ready()&&f.display.cameraWanted&&f.display.flow[unsigned(renderer::Flow::CameraDeferred)]==1,"decoded opaque picture defers global camera withdrawal while an exact active native buffer is still current");
    auto requeue=active.lease.capture;requeue.kind=native::CaptureKind::Requeue;f.source.writer.captureEvent(requeue);f.cycle();
    check(!f.display.camera.ready()&&!f.display.cameraWanted&&f.display.flow[unsigned(renderer::Flow::CameraNone)]==1,"decoded None releases camera after the last current active buffer retires, without a grace timer or stale mask");
    const auto fresh=f.queue();f.cycle();
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!f.display.camera.ready()&&std::chrono::steady_clock::now()<end){f.cycle();std::this_thread::yield();}
    check(f.display.camera.ready()&&f.display.cameraDemand,"fresh exact active decoded picture reenables camera after real opaque output");
    auto ordinary=fresh.lease.capture;ordinary.kind=native::CaptureKind::Requeue;f.source.writer.captureEvent(ordinary);f.cycle();
    check(f.display.camera.ready(),"ordinary native requeue of latest active frame does not create a false opaque demand transition");
    f.source.writer.invalidateCurrentEpoch();f.cycle();
    check(!f.display.camera.ready()&&f.display.flow[unsigned(renderer::Flow::CameraLease)]==1&&!f.display.currentLeases,"actual source epoch loss revokes once and clears the obsolete epoch observation");
    const auto restored=f.queue();(void)restored;f.cycle();
    const auto restoredEnd=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!f.display.camera.ready()&&std::chrono::steady_clock::now()<restoredEnd){f.cycle();std::this_thread::yield();}
    check(f.display.camera.ready()&&f.display.cameraDemand,"same signed source can recover with a fresh exact decoded epoch instead of repeatedly revoking on its old mapping");
}
}
int main(int argc,char** argv){try{const bool handoffOnly=argc==2&&std::strcmp(argv[1],"handoff")==0;if(!handoffOnly){noConsumers();consumerAdmission();sourceLossReconnect();telemetry();decodedDispatch();}exactHandoff();noneAndBurst();interruptedHandoff();cameraHandoff();cameraPictureDemand();std::printf("%u actual process lifecycle and exact handoff checks passed\n",checks);return 0;}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
