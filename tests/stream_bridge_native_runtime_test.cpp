#include "receiver/stream_bridge_native_runtime.h"
#include "receiver/stream_bridge_native_loader.h"
#include "pc/stream-bridge-sender.h"
#include <algorithm>
#include <cstring>
#include <cstdarg>
#include <linux/videodev2.h>
#include <sys/mman.h>
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
    nd::DecodeProvider* ownedNative=nullptr;std::vector<nd::DecodedFrame> decodedFrames;
    std::vector<nd::CaptureObservation> unavailableFrames;nd::SourceIdentity unavailableSource{};
    nd::SnapshotStats counts{};unsigned polls=0,capturePolls=0,randomCalls=0,certified=0,decoded=0;
    bool randomFails=false,routeReady=true,publishGapAfterEmptyPoll=false,leaseCurrent=true;unsigned leaseChecks=0;std::uint64_t clock=100;std::vector<nd::RuntimeNotice> notices;std::vector<nd::CaptureObservation> invalidations;
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
        r.callbacks={onCertified,onDecoded,onCapture,onNotice,this,onUnavailable};return r;}
    nd::SourceProvider provider(){return {poll,pollCapture,stats,random,this,monotonic,currentCapture};}
    static std::uint64_t monotonic(void* v) noexcept{return static_cast<Fixture*>(v)->clock;}
    static bool poll(nd::Observation& out,std::uint8_t* destination,std::size_t capacity,void* v) noexcept {
        auto& f=*static_cast<Fixture*>(v);++f.polls;if(f.ownedNative)return f.ownedNative->poll(out,destination,capacity);if(f.input.empty())return false;const auto& front=f.input.front();if(front.coded.size()>capacity)return false;
        out=front.fact;std::copy(front.coded.begin(),front.coded.end(),destination);f.input.pop_front();return true;}
    static bool pollCapture(nd::CaptureObservation& out,void* v) noexcept {auto& f=*static_cast<Fixture*>(v);++f.capturePolls;if(f.ownedNative)return f.ownedNative->pollCapture(out);if(f.capture.empty()){
        if(f.publishGapAfterEmptyPoll){f.publishGapAfterEmptyPoll=false;++f.counts.dropped;
            f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,300,7,0,0,nd::InputLoss::SnapshotFull,f.counts.dropped});}
        return false;}out=f.capture.front();f.capture.pop_front();return true;}
    static bool currentCapture(const nd::CaptureObservation& fact,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.leaseChecks;if(f.ownedNative)return f.ownedNative->captureLeaseCurrentOnWorker(fact);return f.leaseCurrent&&fact.kind==nd::CaptureKind::Decoded&&fact.fd>=0&&fact.dmabufFd>=0;}
    static nd::SnapshotStats stats(void* v) noexcept{auto& f=*static_cast<Fixture*>(v);return f.ownedNative?f.ownedNative->stats():f.counts;}
    static bool random(void* out,std::size_t n,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.randomCalls;if(f.randomFails)return false;std::memset(out,static_cast<int>(f.randomCalls),n);return true;}
    static void onCertified(const nd::CertifiedFrame& frame,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.certified;f.lastCertified=frame;f.retained=*frame.certificate;f.lastCertified.certificate=&f.retained;}
    static void onDecoded(const nd::DecodedFrame& frame,void* v) noexcept{auto& f=*static_cast<Fixture*>(v);++f.decoded;f.lastDecoded=frame;f.decodedFrames.push_back(frame);}
    static void onCapture(const nd::SourceIdentity&,const nd::CaptureObservation& c,void* v) noexcept{static_cast<Fixture*>(v)->invalidations.push_back(c);}
    static void onUnavailable(const nd::SourceIdentity& source,const nd::CaptureObservation& capture,void* v) noexcept{
        auto& f=*static_cast<Fixture*>(v);f.unavailableFrames.push_back(capture);f.unavailableSource=source;
    }
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
// Real production public-V4L2 observer feeding the real authenticated worker.
// OS returns and original native bytes are owned fixtures; no headset is used.
struct NativeBurst {
    static NativeBurst* active;
    static constexpr std::uintptr_t base=0x100000;
    std::array<std::array<std::uint8_t,4096>,nd::MaxBuffers> storage{};
    std::array<bool,nd::MaxBuffers> queued{};
    nd::DecodeProvider provider;
    unsigned originals=0;
    static bool read(void* out,std::uintptr_t source,std::size_t n,void*) noexcept{
        if(!source||n>4096)return false;
        std::memcpy(out,reinterpret_cast<void*>(source),n);return true;
    }
    static int ioctl(int,unsigned long request,...) {
        ++active->originals;va_list args;va_start(args,request);auto* argument=va_arg(args,void*);va_end(args);
        if(request==VIDIOC_S_FMT)static_cast<v4l2_format*>(argument)->fmt.pix_mp.num_planes=1;
        if(request==VIDIOC_QUERYBUF){auto& b=*static_cast<v4l2_buffer*>(argument);b.m.planes[0].length=4096;
            if(b.type==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)b.m.planes[0].m.mem_offset=b.index*4096;}
        return 0;
    }
    static void* mmap(void*,std::size_t,int,int,int,off_t offset){return active->storage[static_cast<unsigned>(offset)/4096].data();}
    static int munmap(void*,std::size_t){return 0;}static int close(int){return 0;}
    struct Buffer {
        v4l2_buffer b{};v4l2_plane plane{};
        Buffer(unsigned index,bool capture=false){b.index=index;b.type=capture?V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory=capture?V4L2_MEMORY_DMABUF:V4L2_MEMORY_MMAP;b.length=1;b.m.planes=&plane;
            b.flags=V4L2_BUF_FLAG_TIMESTAMP_COPY;plane.length=4096;plane.bytesused=64;}
    };
    NativeBurst(){
        active=this;check(provider.prepare({base,nd::ExpectedVrlinkHash,nd::ExpectedExecutableSegmentHash},{ioctl,mmap,munmap,close},{read,nullptr}),"actual DecodeProvider admits only pinned fixture source/public API originals");
        v4l2_format format{};format.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;format.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_HEVC;
        check(provider.invokeIoctl(base+0x32edc,23,VIDIOC_S_FMT,&format)==0,"native public HEVC OUTPUT format");
        v4l2_requestbuffers buffers{};buffers.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;buffers.memory=V4L2_MEMORY_MMAP;buffers.count=nd::MaxBuffers;
        check(provider.invokeIoctl(base+0x32f18,23,VIDIOC_REQBUFS,&buffers)==0,"native public maximum OUTPUT pool");
        for(unsigned i=0;i<nd::MaxBuffers;++i){Buffer b(i);
            check(provider.invokeIoctl(base+0x32fec,23,VIDIOC_QUERYBUF,&b.b)==0,"native OUTPUT query");
            check(provider.invokeMmap(base+0x33008,nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,23,i*4096)==storage[i].data(),"native OUTPUT owned mmap");}
        format.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        check(provider.invokeIoctl(base+0x3242c,23,VIDIOC_S_FMT,&format)==0,"native public CAPTURE format");
        buffers.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;buffers.memory=V4L2_MEMORY_DMABUF;
        check(provider.invokeIoctl(base+0x324a8,23,VIDIOC_REQBUFS,&buffers)==0,"native public CAPTURE pool");
        for(unsigned i=0;i<nd::MaxBuffers;++i){Buffer b(i,true);check(provider.invokeIoctl(base+0x325c8,23,VIDIOC_QUERYBUF,&b.b)==0,"native CAPTURE query");}
    }
    void input(sb::Bytes coded,unsigned token,unsigned index){
        check(coded.size<=storage[index].size(),"actual encoded certificate fits queried native buffer");
        Buffer b(index);if(queued[index])check(provider.invokeIoctl(base+0x32bc0,23,VIDIOC_DQBUF,&b.b)==0,"native successful OUTPUT reuse");
        std::copy_n(coded.data,coded.size,storage[index].data());b.plane.bytesused=static_cast<unsigned>(coded.size);b.b.timestamp.tv_sec=token;
        const auto before=originals;check(provider.invokeIoctl(base+0x31e0c,23,VIDIOC_QBUF,&b.b)==0&&originals==before+1,"source bytes cross exactly one successful original QBUF");
        queued[index]=true;storage[index].fill(0xee); // Native may immediately reuse memory after handoff.
    }
    void capture(unsigned token,unsigned index){
        Buffer b(index,true);b.b.timestamp.tv_sec=token;b.plane.m.fd=73+static_cast<int>(index);
        check(provider.invokeIoctl(base+0x310f8,23,VIDIOC_QBUF,&b.b)==0,"native CAPTURE lease requeue");
        check(provider.invokeIoctl(base+0x32a5c,23,VIDIOC_DQBUF,&b.b)==0,"native successful current CAPTURE DQBUF");
    }
};
NativeBurst* NativeBurst::active=nullptr;
void actualNativeBurst(){
    Fixture f;auto native=std::make_unique<NativeBurst>();f.ownedNative=&native->provider;
    check(f.worker.prepare(f.request(),f.provider()),"actual provider-to-authenticated-worker pipeline prepares");
    native->input(bytes(ordinary),9,0);f.pump();
    for(unsigned token=10;token<13;++token){const auto coded=f.certifiedDiscovery();native->input(bytes(coded),token,0);native->capture(token,0);f.pump();}
    check(f.sender.confirmed()&&f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.decoded==3,"actual native ownership and early decoder results finish real mutual handshake");
    const auto original=f.worker.status().source;const auto random=f.randomCalls;
    for(unsigned burst=0;burst<4;++burst){
        const auto before=f.decoded;const auto sourceFrame=f.sourceFrame;
        for(unsigned i=0;i<nd::MaxBuffers;++i){const auto coded=f.certifiedFrame();const auto token=100+burst*nd::MaxBuffers+i;
            native->input(bytes(coded),token,i);native->capture(token,i);}
        check(native->provider.stats().dropped==0&&native->provider.stats().rejected==0,"complete native burst never loses owned bytes while worker was unscheduled");
        f.worker.tick();
        check(f.decoded==before+nd::MaxBuffers&&f.worker.immediateWorkPending(),"one bounded production tick joins every early decoded picture and requests immediate backlog check");
        for(unsigned i=0;i<nd::MaxBuffers;++i){const auto& frame=f.decodedFrames[before+i];
            check(frame.descriptor.sourceFrame==sourceFrame+i+1&&frame.submitted.nativeTimestampToken==100+burst*nd::MaxBuffers+i,
                  "every decoded picture has its own exact certificate rather than previous/newest frame");
            check(native->provider.captureLeaseCurrentOnWorker(frame.capture),"every published native CAPTURE lease is still exact/current");}
        f.worker.tick();check(!f.worker.immediateWorkPending(),"drained worker returns to sleeping instead of idle busy loop");
        check(f.worker.status().source.stream.connection==original.stream.connection&&f.randomCalls==random,"sustained complete bursts preserve original confirmed connection");
    }
    f.worker.stop();f.ownedNative=nullptr;
}
void actualNativeTerminalDisposition(){
    Fixture f;auto native=std::make_unique<NativeBurst>();f.ownedNative=&native->provider;
    check(f.worker.prepare(f.request(),f.provider()),"native terminal disposition pipeline prepares");
    native->input(bytes(ordinary),9,0);f.pump();
    for(unsigned token=10;token<13;++token){native->input(bytes(f.certifiedDiscovery()),token,0);native->capture(token,0);f.pump();}
    check(f.sender.confirmed()&&f.decoded==3,"terminal disposition starts from real public native ownership and mutual admission");
    const auto source=f.worker.status().source;
    native->input(bytes(ordinary),200,0);native->capture(200,0);f.pump();
    check(f.unavailableFrames.size()==1&&f.decoded==3&&f.unavailableFrames.back().nativeTimestampToken==200
        &&native->provider.captureLeaseCurrentOnWorker(f.unavailableFrames.back()),"real original QBUF and DQBUF with final ordinary inspection produce one exact still-current negative disposition");
    auto corrupt=f.certifiedFrame();corrupt.back()^=1;native->input(bytes(corrupt),201,1);native->capture(201,1);f.pump();
    check(f.unavailableFrames.size()==2&&f.decoded==3&&f.unavailableFrames.back().nativeTimestampToken==201,
        "real native capture with invalid original-AU proof stays unselected and terminates only its own waiting");
    native->capture(202,2);f.pump();
    check(f.unavailableFrames.size()==2&&f.decoded==3,"actual native DQ ahead of the owned OUTPUT snapshot cannot be declared ordinary");
    native->input(bytes(f.certifiedFrame()),202,2);f.pump();
    check(f.unavailableFrames.size()==2&&f.decoded==4&&f.lastDecoded.capture.nativeTimestampToken==202
        &&f.worker.status().source.stream.connection==source.stream.connection,"actual delayed certified native input publishes its exact capture without resetting the confirmed stream");
    f.worker.stop();f.ownedNative=nullptr;
}

// A real STREAMOFF/STREAMON reset is not REQBUFS: native code retains the
// existing MMAP and DMABUF allocations. Exercise the complete observer ->
// authenticated worker route, not a fabricated new-pool Observation.
void actualNativeRestart(){
    Fixture f;auto native=std::make_unique<NativeBurst>();f.ownedNative=&native->provider;
    check(f.worker.prepare(f.request(),f.provider()),"native restart pipeline prepares");
    native->input(bytes(ordinary),9,0);f.pump();
    for(unsigned token=10;token<13;++token){native->input(bytes(f.certifiedDiscovery()),token,0);native->capture(token,0);f.pump();}
    check(f.sender.confirmed()&&f.decoded==3,"native restart starts from actual confirmed picture ownership");
    const auto original=f.worker.status().source;const auto oldLease=f.lastDecoded.capture;
    const auto port=f.producer.localPort();const auto before=f.decoded;
    int type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    const auto originalCalls=native->originals;
    check(native->provider.invokeIoctl(NativeBurst::base+0x312e8,23,VIDIOC_STREAMOFF,&type)==0
          &&native->originals==originalCalls+1,"ordinary native STREAMOFF forwards exactly once");
    native->queued.fill(false);f.pump();
    check(f.worker.status().state==nd::RuntimeState::SourceLost
          &&!native->provider.captureLeaseCurrentOnWorker(oldLease),"STREAMOFF immediately revokes old session and exact picture lease");
    check(f.worker.status().lastNativeReset==nd::ResetReason::OutputStreamOff,
          "bounded worker notice reports the actual native stop cause rather than an unexplained source loss");
    check(native->provider.invokeIoctl(NativeBurst::base+0x31368,23,VIDIOC_STREAMON,&type)==0,"native STREAMON reuses allocated buffers");
    f.relisten(port);
    native->input(bytes(ordinary),9,0);f.pump();
    check(f.randomCalls==2&&f.worker.status().source.outputPoolGeneration>original.outputPoolGeneration,
          "fresh actual QBUF after STREAMON starts a new authenticated source without REQBUFS or remapping");
    check(f.decoded==before,"STREAMON and ordinary input alone never reuse an old mask");
    for(unsigned token=10;token<13;++token){native->input(bytes(f.certifiedDiscovery()),token,0);native->capture(token,0);f.pump();}
    check(f.sender.confirmed()&&f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.decoded==before+3,
          "real native reset resumes new exact certified pictures through full mutual admission");
    check(f.worker.status().source.stream.connection!=original.stream.connection
          &&!native->provider.captureLeaseCurrentOnWorker(oldLease),"new source never revives previous session or CAPTURE lease");
    const auto resumed=f.worker.status().source;const auto admissions=f.randomCalls;
    for(unsigned token=20;token<52;++token){native->input(bytes(f.certifiedFrame()),token,token%nd::MaxBuffers);native->capture(token,token%nd::MaxBuffers);f.pump();}
    check(f.decoded==before+35&&f.randomCalls==admissions
          &&f.worker.status().source.stream.connection==resumed.stream.connection,
          "post-reset continuous exact frames keep one stable session rather than repeated renegotiation");
    f.worker.stop();f.ownedNative=nullptr;
}

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
void ordinaryWithoutCapture(){
    Fixture f;f.negotiate();const auto before=f.certified;
    // Public stateful V4L2 explicitly allows OUTPUTs with no CAPTURE result.
    // These ordinary AUs have no authenticated picture receipt to retain.
    for(unsigned i=0;i<nd::ReceiverWorker::PendingCapacity+8;++i){f.enqueue(ordinary,100+i);f.pump();}
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.certified==before,
        "ordinary no-CAPTURE inputs never exhaust certified pending receipts");
    f.enqueue(f.certifiedFrame(),200);f.pump();f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,200,7,21,200});f.pump();
    check(f.certified==before+1&&f.decoded==4,"fresh exact certified picture survives an arbitrary ordinary input run");
}
void terminalCaptureDisposition(){
    Fixture f;f.negotiate();f.unavailableFrames.clear();const auto before=f.decoded,certified=f.certified;
    const nd::CaptureObservation early{nd::CaptureKind::Decoded,23,73,0,40,7,21,40};
    f.capture.push_back(early);f.pump();
    check(f.unavailableFrames.empty()&&f.decoded==before,"a pending owned input never creates a terminal negative capture disposition");
    f.enqueue(f.certifiedFrame(),40);f.pump();
    check(f.unavailableFrames.empty()&&f.decoded==before+1,"late exact authenticated input still publishes its early capture without a negative shortcut");
    auto ordinaryCapture=early;ordinaryCapture.nativeTimestampToken=41;ordinaryCapture.contentGeneration=41;
    f.enqueue(ordinary,41);f.pump();f.capture.push_back(ordinaryCapture);f.pump();
    check(f.unavailableFrames.size()==1&&f.unavailableFrames.back().nativeTimestampToken==41&&f.unavailableFrames.back().contentGeneration==41
        &&f.unavailableSource.stream.connection==f.worker.status().source.stream.connection&&f.unavailableSource.outputPoolGeneration==7
        &&f.decoded==before+1&&f.certified==certified+1,"final ordinary OUTPUT rejection closes only its exact current capture without certifying a picture");
    auto earlyOrdinary=ordinaryCapture;earlyOrdinary.nativeTimestampToken=42;earlyOrdinary.contentGeneration=42;
    f.capture.push_back(earlyOrdinary);f.pump();
    check(f.unavailableFrames.size()==1,"early ordinary-looking capture waits for actual input inspection");
    f.enqueue(ordinary,42);f.pump();
    check(f.unavailableFrames.size()==2&&f.unavailableFrames.back().nativeTimestampToken==42,"ordinary input inspection terminates the exact capture that arrived before its owned bytes");
    auto invalid=earlyOrdinary;invalid.nativeTimestampToken=43;invalid.contentGeneration=43;
    auto corrupt=f.certifiedFrame();corrupt.back()^=1;f.capture.push_back(invalid);f.pump();f.enqueue(corrupt,43);f.pump();
    check(f.unavailableFrames.size()==3&&f.unavailableFrames.back().nativeTimestampToken==43&&f.decoded==before+1&&f.certified==certified+1,
        "invalid original-AU proof remains unselected while its exact rejected capture stops waiting");
    auto stale=invalid;stale.nativeTimestampToken=44;stale.contentGeneration=44;f.enqueue(ordinary,44);f.pump();f.leaseCurrent=false;
    f.capture.push_back(stale);f.pump();check(f.unavailableFrames.size()==3,"retired native storage cannot receive a fresh terminal capture disposition");f.leaseCurrent=true;
    auto gap=stale;gap.nativeTimestampToken=45;gap.contentGeneration=45;f.capture.push_back(gap);f.pump();
    ++f.counts.dropped;f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,45,7,0,0,nd::InputLoss::SnapshotFull,1});f.pump();
    check(f.unavailableFrames.size()==4&&f.unavailableFrames.back().nativeTimestampToken==45&&f.worker.status().state==nd::RuntimeState::PeerConfirmed,
        "exact known input loss closes its early capture while independent certified source authority remains live");
    auto unknown=gap;unknown.nativeTimestampToken=46;unknown.contentGeneration=46;f.capture.push_back(unknown);f.pump();++f.counts.captureDropped;f.pump();
    f.enqueue(ordinary,46);f.pump();
    check(f.unavailableFrames.size()==4&&f.worker.status().state==nd::RuntimeState::SourceLost,"unknown capture loss retains emergency source revocation without inventing an ordinary disposition");
}

void earlyDecodedOrdering(){
    Fixture f;f.negotiate();const auto before=f.decoded;
    f.enqueue(f.certifiedFrame(),40);
    f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});
    f.pump();
    check(f.decoded==before+1,"owned input and native decoded result queued before one worker tick must join exact picture once");
}
void earlyDecodedValidity(){
    // A decoded result may precede worker admission of its owned coded bytes.
    // Holding that scalar fact never extends the actual native image lease.
    {
        Fixture f;f.negotiate();const auto before=f.decoded;
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
        check(f.decoded==before,"early native result alone cannot invent a certificate");
        f.leaseCurrent=false;f.enqueue(f.certifiedFrame(),40);f.pump();
        check(f.decoded==before&&f.leaseChecks>=2,"native lease is revalidated at publication, not merely on early retention");
        f.leaseCurrent=true;f.enqueue(f.certifiedFrame(),40);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,41});f.pump();
        check(f.decoded==before,"a stale early lease permanently quarantines its ambiguous exact token");
        f.enqueue(f.certifiedFrame(),41);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,41,7,21,42});f.pump();
        check(f.decoded==before+1,"stale early lease does not revoke a different exact valid picture");
    }
    {
        Fixture f;f.negotiate();const auto before=f.decoded;
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
        f.capture.push_back({nd::CaptureKind::Requeue,23,73,0,0,7,21,41});f.pump();
        f.enqueue(f.certifiedFrame(),40);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,42});f.pump();
        check(f.decoded==before,"native requeue retires an early fact before a late certificate can use it");
        f.enqueue(f.certifiedFrame(),41);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,41,7,21,43});f.pump();
        check(f.decoded==before+1,"requeue preserves the unrelated next native cycle");
    }
    {
        Fixture f;f.negotiate();const auto before=f.decoded;const auto source=f.worker.status().source;
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,65535,7,21,40});f.pump();
        ++f.counts.dropped;f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,65535,7,0,0,nd::InputLoss::SnapshotFull,1});f.pump();
        f.enqueue(f.certifiedFrame(),65535);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,65535,7,21,41});f.pump();
        check(f.decoded==before,"known loss retires an early result even at the 16-bit token boundary");
        f.enqueue(f.certifiedFrame(),0);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,0,7,21,42});f.pump();
        check(f.decoded==before+1&&f.worker.status().source.stream.connection==source.stream.connection,
              "token wrap to a different exact token preserves current transport and publishes correctly");
    }
    {
        Fixture f;f.negotiate();const auto before=f.decoded;
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
        const auto oldCertificate=f.certifiedFrame();
        f.capture.push_back({nd::CaptureKind::SourceReset,23,-1,0,0,7,22,0});f.pump();
        f.enqueue(ordinary,41,8);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.enqueue(oldCertificate,40,8);f.pump();
        check(f.decoded==before&&f.randomCalls==2&&f.worker.status().source.stream.generation==8,
              "retired source pool facts cannot join a later source with the same fd and timestamp token");
    }
    {
        Fixture f;f.negotiate();const auto before=f.decoded;
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
        const auto oldCertificate=f.certifiedFrame();
        ++f.counts.captureDropped;f.pump();f.enqueue(oldCertificate,40);f.pump();
        check(f.decoded==before&&f.worker.status().state==nd::RuntimeState::SourceLost,
              "unknown CAPTURE event loss remains a hard global stop even with a retained early fact");
    }
}

void lifecycleAndLoss(){
    Fixture f;f.negotiate();const auto before=f.certified;
    f.capture.push_back({nd::CaptureKind::Reset,23,-1,0,0,7,22,0});f.pump();
    check(f.worker.status().state==nd::RuntimeState::SourceLost&&f.worker.status().reason==nd::RuntimeReason::CapturePoolReset,
        "capture-only reset revokes previous authority before a fresh same-pool admission");
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
void knownGapRecovery(){
    Fixture f;f.negotiate();const auto source=f.worker.status().source;
    f.enqueue(f.certifiedFrame(),40);f.pump();const auto certified=f.certified,decoded=f.decoded;
    const auto notices=f.notices.size();
    ++f.counts.dropped;f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,41,7,0,0,nd::InputLoss::SnapshotFull,1});f.pump();
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.randomCalls==1
        &&f.worker.status().source.stream.connection==source.stream.connection
        &&f.worker.status().source.stream.generation==source.stream.generation,"fully scoped gap preserves confirmed source control and nonce");
    bool reported=false;for(std::size_t i=notices;i<f.notices.size();++i){
        check(f.notices[i].state!=nd::RuntimeState::SourceLost&&f.notices[i].state!=nd::RuntimeState::SenderUnavailable,"known gap cannot globally revoke current image epoch");
        reported|=f.notices[i].reason==nd::RuntimeReason::SnapshotGap&&f.notices[i].statistics.dropped==1;
    }
    check(reported,"bounded notice retains exact known gap cause/counter without disconnect");
    check(f.invalidations.empty()||f.invalidations.back().kind!=nd::CaptureKind::InputGap,"OUTPUT gap is not a CAPTURE invalidation");
    f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
    check(f.decoded==decoded+1,"unrelated already certified pending frame remains usable across another token gap");
    for(unsigned attempt=0;attempt<3;++attempt){
        f.enqueue(f.certifiedFrame(),41);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,41,7,21,900+attempt});f.pump();
    }
    check(f.certified==certified&&f.decoded==decoded+1,"every late completion and wrapped reuse of lost exact token remains quarantined");
    f.enqueue(f.certifiedFrame(),50);f.pump();const auto pendingCount=f.certified;
    ++f.counts.dropped;f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,50,7,0,0,nd::InputLoss::SnapshotFull,2});f.pump();
    f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,50,7,21,1000});f.pump();
    f.enqueue(f.certifiedFrame(),50);f.pump();
    check(f.certified==pendingCount&&f.decoded==decoded+1,"a known gap colliding with an existing pending token retires only that ambiguous token");
    ++f.counts.rejected;f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,51,7,0,0,nd::InputLoss::CopyRejected,1});f.pump();
    f.enqueue(f.certifiedFrame(),51);f.pump();
    check(f.certified==pendingCount&&f.randomCalls==1,"known owned-copy failure has same exact quarantine without transport reset");
    f.enqueue(f.certifiedFrame(),60);f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,60,7,21,60});f.pump();
    check(f.decoded==decoded+2&&f.worker.status().state==nd::RuntimeState::PeerConfirmed,"next intact real certificate and early decode join immediately without new handshake");
    Fixture raced;raced.negotiate();raced.publishGapAfterEmptyPoll=true;raced.worker.tick();raced.worker.tick();
    check(raced.randomCalls==1&&raced.worker.status().state==nd::RuntimeState::PeerConfirmed,"gap published after empty drain stays covered without false unknown loss or reconnect");
    Fixture unknown;unknown.negotiate();++unknown.counts.dropped;unknown.pump();unknown.clock+=10000;
    unknown.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,80,7,0,0,nd::InputLoss::SnapshotFull,1});unknown.pump();
    unknown.enqueue(ordinary,81);unknown.pump();check(unknown.randomCalls==1&&unknown.worker.status().reason==nd::RuntimeReason::UnknownSnapshotLoss,"late known fact cannot resurrect prior unknown loss");
    unknown.enqueue(ordinary,82,8);unknown.pump();check(unknown.randomCalls==1,"unknown global loss survives later source pools");
    Fixture overflow;overflow.negotiate();++overflow.counts.captureDropped;++overflow.counts.dropped;
    overflow.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,90,7,0,0,nd::InputLoss::SnapshotFull,1});overflow.pump();
    check(overflow.worker.status().reason==nd::RuntimeReason::CaptureEventsLost,"event overflow remains hard refused despite scoped gap");
    Fixture missing;missing.negotiate();missing.counts.dropped=2;
    missing.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,100,7,0,0,nd::InputLoss::SnapshotFull,2});missing.pump();
    check(missing.worker.status().state==nd::RuntimeState::SourceLost,"noncontiguous exact-loss cursor remains global refusal");
}

void routeQuarantineAndBackoff(){
    Fixture f;f.negotiate();const auto port=f.producer.localPort();
    f.routeReady=false;f.enqueue(ordinary,70);f.pump();++f.counts.dropped;
    f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,71,7,0,0,nd::InputLoss::SnapshotFull,1});f.pump();
    f.relisten(port);f.routeReady=true;f.pump();f.enqueue(ordinary,80);f.pump();
    for(std::uint32_t token=81;token<84;++token){f.enqueue(f.certifiedDiscovery(),token);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,21,token});f.pump();}
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed,"route regain requires a fresh actual certified handshake");
    const auto before=f.certified;
    ++f.counts.dropped;f.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,1,6,0,0,nd::InputLoss::SnapshotFull,2});f.pump();
    for(unsigned token:{70U,71U}){f.enqueue(f.certifiedFrame(),token);f.pump();}
    check(f.certified==before&&f.worker.status().state==nd::RuntimeState::PeerConfirmed,
        "older pool gap never clears newer paused-route token quarantine or resets active admission");
    Fixture unknown;unknown.negotiate();const auto unknownPort=unknown.producer.localPort();unknown.routeReady=false;unknown.pump();
    ++unknown.counts.rejected;unknown.pump();unknown.relisten(unknownPort);unknown.routeReady=true;unknown.pump();
    unknown.enqueue(ordinary,90);unknown.pump();check(unknown.randomCalls==1,"unknown loss during paused route cannot be hidden by route baseline replacement");
    Fixture retry;retry.negotiate();const auto retryPort=retry.producer.localPort();
    retry.producer.stop();retry.pump();
    retry.relisten(retryPort);retry.clock+=249;
    ++retry.counts.dropped;retry.capture.push_back({nd::CaptureKind::InputGap,23,-1,2,101,7,0,0,nd::InputLoss::SnapshotFull,1});retry.pump();
    retry.clock+=1;retry.enqueue(ordinary,102);retry.pump();
    check(retry.randomCalls==2,"additional known gap cannot postpone an already scheduled bounded transport retry");
    Fixture reset;reset.negotiate();const auto resetPort=reset.producer.localPort();const auto previous=reset.worker.status().source;
    reset.capture.push_back({nd::CaptureKind::Reset,23,-1,0,0,7,22,0});reset.pump();reset.relisten(resetPort);reset.clock+=250;
    reset.enqueue(ordinary,110);reset.pump();
    for(std::uint32_t token=111;token<114;++token){reset.enqueue(reset.certifiedDiscovery(),token);reset.pump();
        reset.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,22,token});reset.pump();}
    check(reset.worker.status().state==nd::RuntimeState::PeerConfirmed&&reset.worker.status().source.stream.connection!=previous.stream.connection,
        "known capture-pool reset recovers only by fresh full admission, never old session authority");
    Fixture recycled;recycled.negotiate();recycled.capture.push_back({nd::CaptureKind::SourceReset,23,-1,0,0,7,23,0});recycled.pump();
    const auto recyclePort=recycled.producer.localPort();recycled.relisten(recyclePort);
    recycled.enqueue(ordinary,120,8);recycled.input.back().fact.fd=24;recycled.pump();
    recycled.capture.push_back({nd::CaptureKind::SourceReset,24,-1,0,0,8,24,0});recycled.pump();recycled.relisten(recyclePort);
    recycled.enqueue(ordinary,121,9);recycled.input.back().fact.fd=25;recycled.pump();
    check(recycled.randomCalls==3,"exact native OUTPUT retirements release bounded tracking for later decoder file descriptors");
    recycled.enqueue(ordinary,122,7);recycled.pump();check(recycled.randomCalls==3,"exact retired source epoch cannot reenter after fd tracking was released");
    Fixture concurrent;concurrent.negotiate();concurrent.enqueue(ordinary,140);concurrent.pump();
    concurrent.capture.push_back({nd::CaptureKind::SourceReset,24,-1,0,0,20,21,0});concurrent.pump();
    const auto concurrentCertified=concurrent.certified;
    concurrent.enqueue(concurrent.certifiedFrame(),140);concurrent.pump();
    concurrent.enqueue(concurrent.certifiedFrame(),141);concurrent.pump();
    concurrent.capture.push_back({nd::CaptureKind::Decoded,23,73,0,141,7,21,141});concurrent.pump();
    check(concurrent.certified==concurrentCertified+1&&concurrent.decoded==4&&concurrent.worker.status().state==nd::RuntimeState::PeerConfirmed,
        "retiring newer decoder B never clears older live decoder A quarantine or prevents its fresh exact frames");
    concurrent.enqueue(ordinary,142,20);concurrent.input.back().fact.fd=24;concurrent.pump();
    check(concurrent.randomCalls==1,"retired decoder B cannot reactivate from a stale observation");
    Fixture bounded;bounded.negotiate();
    for(std::size_t i=0;i<nd::ReceiverWorker::RetiredPoolCapacity+1;++i){bounded.capture.push_back({nd::CaptureKind::SourceReset,static_cast<int>(24+i),-1,0,0,100+i,200+i,0});bounded.pump();}
    check(bounded.worker.status().state==nd::RuntimeState::SourceLost&&bounded.worker.status().reason==nd::RuntimeReason::QuarantineCapacity,
        "per-fd source-retirement tombstones refuse on bounded capacity without unsafe eviction");
    const auto boundedCalls=bounded.randomCalls;bounded.enqueue(ordinary,150,1000);bounded.input.back().fact.fd=26;bounded.pump();
    bounded.enqueue(ordinary,151,1001);bounded.pump();
    check(bounded.randomCalls==boundedCalls,"retirement capacity exhaustion remains globally refused across other fd and new-pool inputs");
}

void liveDecoderSwitch(){
    Fixture f;f.negotiate();const auto port=f.producer.localPort();
    f.enqueue(f.certifiedFrame(),40);f.pump();const auto before=f.certified;
    f.relisten(port);f.enqueue(ordinary,50,20);f.input.back().fact.fd=24;f.pump();
    for(unsigned token=51;token<54;++token){f.enqueue(f.certifiedDiscovery(),token,20);f.input.back().fact.fd=24;f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,24,74,0,token,20,21,token});f.pump();}
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed&&f.worker.status().source.decoderFd==24,"second still-live decoder gets its own actual full admission");
    f.relisten(port);f.enqueue(ordinary,60);f.pump();
    for(unsigned token=61;token<64;++token){f.enqueue(f.certifiedDiscovery(),token);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,7,21,token});f.pump();}
    const auto fresh=f.certified;f.enqueue(f.certifiedFrame(),40);f.pump();
    f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,40,7,21,40});f.pump();
    check(f.certified==fresh&&fresh==before+6&&f.decoded==9,
        "switching live decoder A B A quarantines old A pending token before dropping its prior session");
}

void sameFdPoolOrdering(){
    Fixture f;f.negotiate();const auto port=f.producer.localPort();
    f.enqueue(f.certifiedFrame(),40);f.pump();
    // The provider's successful new-pool input may become visible after this
    // tick's first capture drain, before its SourceReset event is next drained.
    f.relisten(port);f.enqueue(ordinary,50,8);f.pump();
    check(f.randomCalls==2&&f.worker.status().source.outputPoolGeneration==8
        &&f.worker.status().state==nd::RuntimeState::Negotiating,
        "new same-fd pool retires prior pending before advancing quarantine metadata");
    for(unsigned token=51;token<54;++token){f.enqueue(f.certifiedDiscovery(),token,8);f.pump();
        f.capture.push_back({nd::CaptureKind::Decoded,23,73,0,token,8,22,token});f.pump();}
    check(f.worker.status().state==nd::RuntimeState::PeerConfirmed,"genuinely new same-fd pool remains live after source-event scheduling boundary");
    const auto count=f.certified;f.enqueue(ordinary,60,7);f.pump();
    check(f.certified==count&&f.worker.status().source.outputPoolGeneration==8,"delayed old same-fd pool never rolls back the newer quarantine");
}

void ambiguityAndBound(){
    Fixture f;f.start();f.enqueue(f.certifiedDiscovery(),30);f.pump();check(f.certified==1,"first unique token admitted");
    f.enqueue(ordinary,30);f.pump();check(f.worker.status().state==nd::RuntimeState::SourceLost,"same live native token never aliases a newer source frame");
    Fixture full;full.negotiate();for(unsigned i=0;i<nd::ReceiverWorker::PendingCapacity+1;++i){full.enqueue(full.certifiedFrame(),100+i);full.pump();}
    check(full.worker.status().state==nd::RuntimeState::SourceLost,"in-flight table capacity refuses without eviction");
    Fixture random;random.randomFails=true;random.start();random.enqueue(ordinary,70);random.pump();
    check(random.worker.status().state==nd::RuntimeState::SourceRefused&&random.certified==0,"failed source-session OS randomness never uses weak ID fallback");
    Fixture pairing;auto r=pairing.request();r.pairingsDirectory=pairing.files.root/"missing";check(pairing.worker.prepare(r,pairing.provider()),"missing credentials checked on worker after real source");
    pairing.enqueue(ordinary,71);pairing.pump();check(pairing.worker.status().state==nd::RuntimeState::PairingRefused,"missing selected receiver credential gets bounded status");
}
void imageRouteAdmission(){
    Fixture waiting;waiting.routeReady=false;waiting.start();
    check(waiting.worker.status().state==nd::RuntimeState::WaitingImageRoute&&waiting.randomCalls==0&&waiting.polls>0,
        "unprepared final image route starts no source identity or control while retiring observed native tokens");
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
int main(int argc,char** argv){try{if(argc==2&&std::strcmp(argv[1],"--native-restart")==0){actualNativeRestart();std::cout<<checks<<" actual native restart checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--native-burst")==0){actualNativeBurst();std::cout<<checks<<" actual native burst checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--early-validity")==0){earlyDecodedValidity();std::cout<<checks<<" early decoded validity checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--early-decoded")==0){earlyDecodedOrdering();std::cout<<checks<<" early decoded checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--pool-order")==0){sameFdPoolOrdering();std::cout<<checks<<" pool-order checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--route-gaps")==0){routeQuarantineAndBackoff();std::cout<<checks<<" route-gap checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--live-switch")==0){liveDecoderSwitch();std::cout<<checks<<" live-switch checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--known-gap")==0){knownGapRecovery();std::cout<<checks<<" known-gap checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--native-disposition")==0){actualNativeTerminalDisposition();std::cout<<checks<<" actual native terminal disposition checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--capture-disposition")==0){terminalCaptureDisposition();std::cout<<checks<<" terminal capture disposition checks PASS\n";return 0;}if(argc==2&&std::strcmp(argv[1],"--ordinary-gap")==0){ordinaryWithoutCapture();std::cout<<checks<<" no-capture checks PASS\n";return 0;}offAndPreparation();actualNativeBurst();actualNativeTerminalDisposition();actualNativeRestart();earlyDecodedOrdering();earlyDecodedValidity();realVideoAndCapture();unprovedInputs();ordinaryWithoutCapture();terminalCaptureDisposition();knownGapRecovery();routeQuarantineAndBackoff();liveDecoderSwitch();sameFdPoolOrdering();lifecycleAndLoss();ambiguityAndBound();imageRouteAdmission();lateTransportReadiness();transportRecoveryRejectsOldProof();std::cout<<checks<<" hosted native receiver checks PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
