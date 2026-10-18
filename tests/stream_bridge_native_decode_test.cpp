#include "../receiver/stream_bridge_native_decode.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <linux/videodev2.h>
#include <sys/mman.h>
namespace nd=stream_bridge_native;
namespace {
unsigned checks=0;
void check(bool v,const char* label){++checks;if(!v)throw std::runtime_error(label);}
}
#ifdef SFNP_NATIVE_DECODE_ENTRY_TEST
namespace {
struct Result{std::uint64_t v[4];};
using Wide=Result(*)(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,
    std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,
    double,double,double,double,double,double,double,double,double,double);
unsigned classified=0;
Result wide(std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,std::uint64_t e,
    std::uint64_t f,std::uint64_t g,std::uint64_t h,std::uint64_t i,std::uint64_t j,
    double p,double q,double r,double s,double t,double u,double v,double w,double x,double y){
    check(a==11&&b==22&&c==33&&d==44&&e==55&&f==66&&g==77&&h==88,"all integer argument registers preserved");
    check(i==99&&j==110,"original stack argument area preserved");
    check(p==1.5&&q==2.5&&r==3.5&&s==4.5&&t==5.5&&u==6.5&&v==7.5&&w==8.5,"all vector argument registers preserved");
    check(x==9.5&&y==10.5,"vector overflow stack args preserved");
    check(errno==EALREADY,"classifier preserves original errno before tail handoff");
    errno=ENOSPC;return {{0x123456789abcdef0ULL,77,88,99}};
}
}
extern "C" void sfnp_stream_decode_ioctl_entry();
extern "C" void* sfnp_native_decode_classify(int,unsigned long,void*,std::uintptr_t returnAddress) noexcept{
    const int before=errno;++classified;if(!returnAddress)std::abort();errno=EDOM;
    __asm__ __volatile__("movi v0.16b,#0\n movi v1.16b,#0\n movi v2.16b,#0\n movi v3.16b,#0\n"
        "movi v4.16b,#0\n movi v5.16b,#0\n movi v6.16b,#0\n movi v7.16b,#0":::
        "v0","v1","v2","v3","v4","v5","v6","v7");
    auto target=&wide;void* output=nullptr;static_assert(sizeof target==sizeof output,"LP64");
    std::memcpy(&output,&target,sizeof output);errno=before;return output;
}
int main(){try{
    auto entry=&sfnp_stream_decode_ioctl_entry;Wide fn=nullptr;std::memcpy(&fn,&entry,sizeof fn);errno=EALREADY;
    const auto r=fn(11,22,33,44,55,66,77,88,99,110,1.5,2.5,3.5,4.5,5.5,6.5,7.5,8.5,9.5,10.5);
    check(classified==1,"one classifier invocation only");
    check(r.v[0]==0x123456789abcdef0ULL&&r.v[1]==77&&r.v[2]==88&&r.v[3]==99,"hidden result pointer x8 preserved");
    check(errno==ENOSPC,"exact target errno preserved to original caller");
    std::printf("AArch64 genuine tail-forward: %u checks PASS\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
#else
namespace {
constexpr std::uintptr_t base=0x100000;
struct Fake{
    std::array<std::array<std::uint8_t,4096>,nd::MaxBuffers> memory{};
    unsigned ioctlCalls=0,mapCalls=0,unmapCalls=0,closeCalls=0,reads=0,dataReads=0;
    int result=0,error=EOVERFLOW,receivedErrno=0;std::uint32_t dequeue=0;
    bool readFails=false,mapFails=false,zeroCopyDuringQbuf=false;
    bool requireCaptureSink=false;unsigned sinkCalls=0;nd::CaptureObservation lastSink{};
    nd::DecodeProvider* revokeDuringQbuf=nullptr;
} fake;
bool copy(void* output,std::uintptr_t source,std::size_t n,void*) noexcept{
    ++fake.reads;errno=EDOM;
    if(fake.readFails||source<4096||n>4096)return false;
    const auto* start=reinterpret_cast<const std::uint8_t*>(fake.memory.data());
    if(source>=reinterpret_cast<std::uintptr_t>(start)&&source<reinterpret_cast<std::uintptr_t>(start)+sizeof fake.memory)++fake.dataReads;
    std::memcpy(output,reinterpret_cast<const void*>(source),n);return true;
}
int originalIoctl(int fd,unsigned long request,...){
    ++fake.ioctlCalls;fake.receivedErrno=errno;va_list args;va_start(args,request);void* p=va_arg(args,void*);va_end(args);
    check(fd==23||fd==24,"original fd exact");
    if(fake.result==0){
        if(request==VIDIOC_S_FMT)static_cast<v4l2_format*>(p)->fmt.pix_mp.num_planes=1;
        if(request==VIDIOC_QUERYBUF){auto& b=*static_cast<v4l2_buffer*>(p);b.m.planes[0].length=4096;b.m.planes[0].m.mem_offset=b.index*4096;}
        if(request==VIDIOC_DQBUF)static_cast<v4l2_buffer*>(p)->index=fake.dequeue;
        if(request==VIDIOC_QBUF){
            auto& b=*static_cast<v4l2_buffer*>(p);
            if(fake.zeroCopyDuringQbuf&&b.index<fake.memory.size())fake.memory[b.index].fill(0xee);
            if(fake.revokeDuringQbuf)fake.revokeDuringQbuf->stop();
        }
    }
    if(request==VIDIOC_QBUF&&static_cast<v4l2_buffer*>(p)->type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE&&fake.requireCaptureSink)
        check(fake.sinkCalls&&fake.lastSink.kind==nd::CaptureKind::Requeue,"synchronous registered sink invalidates capture before original QBUF even on failure");
    errno=fake.error;return fake.result;
}
void* originalMmap(void* address,std::size_t size,int prot,int flags,int fd,off_t offset){
    ++fake.mapCalls;fake.receivedErrno=errno;
    check(address==nullptr&&size==4096&&prot==(PROT_READ|PROT_WRITE)&&flags==MAP_SHARED&&fd==23&&offset>=0,"original mmap arguments exact");
    errno=fake.error;return fake.mapFails?MAP_FAILED:fake.memory[static_cast<std::size_t>(offset)/4096].data();
}
int originalMunmap(void*,std::size_t){++fake.unmapCalls;fake.receivedErrno=errno;errno=fake.error;return fake.result;}
int originalClose(int fd){++fake.closeCalls;fake.receivedErrno=errno;check(fd==23,"original close fd exact");errno=fake.error;return fake.result;}
nd::Originals calls(){return {originalIoctl,originalMmap,originalMunmap,originalClose};}
nd::ImageIdentity image(){return {base,nd::ExpectedVrlinkHash,nd::ExpectedExecutableSegmentHash};}
std::unique_ptr<nd::DecodeProvider> provider(){fake=Fake{};auto p=std::make_unique<nd::DecodeProvider>();check(p->prepare(image(),calls(),{copy,nullptr}),"exact pinned source and public originals prepared");return p;}
v4l2_format format(){v4l2_format f{};f.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;f.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_HEVC;return f;}
void setup(nd::DecodeProvider& p,unsigned count=4){
    auto f=format();errno=EALREADY;const auto before=fake.ioctlCalls;
    check(p.invokeIoctl(base+0x32edc,23,VIDIOC_S_FMT,&f)==0&&errno==fake.error&&fake.receivedErrno==EALREADY,"S_FMT result and errno exact despite observer reads");
    check(fake.ioctlCalls==before+1,"S_FMT original exactly once");
    v4l2_requestbuffers r{};r.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;r.memory=V4L2_MEMORY_MMAP;r.count=count;
    check(p.invokeIoctl(base+0x32f18,23,VIDIOC_REQBUFS,&r)==0,"actual REQBUFS allocation");
}
struct Buffer{
    v4l2_buffer b{};v4l2_plane plane{};
    explicit Buffer(unsigned index){b.index=index;b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;b.memory=V4L2_MEMORY_MMAP;b.length=1;b.m.planes=&plane;b.flags=V4L2_BUF_FLAG_TIMESTAMP_COPY;b.timestamp.tv_sec=0x12345678;plane.length=4096;plane.bytesused=64;}
};
void map(nd::DecodeProvider& p,unsigned index){
    Buffer b(index);check(p.invokeIoctl(base+0x32fec,23,VIDIOC_QUERYBUF,&b.b)==0,"QUERYBUF successful original");
    errno=EALREADY;
    check(p.invokeMmap(base+0x33008,nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,23,index*4096)==fake.memory[index].data()
        &&errno==fake.error&&fake.receivedErrno==EALREADY,"QUERYBUF offset/length bound to successful mmap, exact result+errno");
}
int queue(nd::DecodeProvider& p,unsigned index){Buffer b(index);errno=EALREADY;const auto before=fake.ioctlCalls;const auto result=p.invokeIoctl(base+0x31e0c,23,VIDIOC_QBUF,&b.b);
    check(fake.ioctlCalls==before+1&&fake.receivedErrno==EALREADY&&errno==fake.error,"QBUF original once with exact errno");return result;}
void dequeue(nd::DecodeProvider& p,unsigned index){fake.dequeue=index;Buffer b(0);check(p.invokeIoctl(base+0x32bc0,23,VIDIOC_DQBUF,&b.b)==0,"output DQBUF successful original");}
void noObservation(nd::DecodeProvider& p,const char* label){nd::Observation fact{};fact.usedBytes=123;std::array<std::uint8_t,128> out{};out.fill(0x87);const auto saved=out;check(!p.poll(fact,out.data(),out.size())&&fact.usedBytes==123&&out==saved,label);}
nd::Observation poll(nd::DecodeProvider& p,std::uint8_t expected){nd::Observation fact{};std::array<std::uint8_t,128> out{};
    check(p.poll(fact,out.data(),out.size())&&fact.usedBytes==64,"successful source chunk copied into caller-owned storage");
    check(std::all_of(out.begin(),out.begin()+64,[expected](auto b){return b==expected;}),"snapshot copies bytes before native QBUF handoff");
    check(!fact.completeAccessUnit&&fact.nativeTimestampToken==0x12345678&&fact.poolGeneration&&fact.contentGeneration,"native timestamp preserved independently; no invented complete-AU flag");
    check(fact.bufferCapacity==4096,"native capacity comes from exact queried/mapped buffer, not snapshot limit");return fact;}
void identityAndOwnership(){
    auto p=provider();auto wrong=image();wrong.file[0]^=1;
    check(!p->prepare(wrong,calls(),{copy,nullptr}),"wrong exact executable refuses admission");
    wrong=image();wrong.executableSegment[0]^=1;check(!p->prepare(wrong,calls(),{copy,nullptr}),"loaded executable must match independent RX hash");
    check(p->knownIoctl(base+0x31e0c,VIDIOC_QBUF)&&!p->knownIoctl(base+0x31e0c,VIDIOC_DQBUF)&&!p->knownIoctl(base+0x31e10,VIDIOC_QBUF),"exact return address and public request pair required");
    setup(*p);queue(*p,0);noObservation(*p,"no query/mmap no pointer reading");dequeue(*p,0);map(*p,0);
    fake.memory[0].fill(0x42);fake.zeroCopyDuringQbuf=true;queue(*p,0);
    std::array<std::uint8_t,8> small{};small.fill(0x29);nd::Observation untouched{};untouched.usedBytes=999;
    check(!p->poll(untouched,small.data(),small.size())&&untouched.usedBytes==999&&small[0]==0x29,"too-small poll is transactional and retains snapshot");
    auto first=poll(*p,0x42);queue(*p,0);noObservation(*p,"kernel-owned queued buffer cannot be recopied");
    dequeue(*p,0);fake.memory[0].fill(0x53);queue(*p,0);const auto second=poll(*p,0x53);
    check(first.poolGeneration==second.poolGeneration&&second.contentGeneration>first.contentGeneration,"DQBUF re-use gains distinct content generation without buffer-index identity guess");
    dequeue(*p,0);fake.result=-1;fake.error=EIO;queue(*p,0);noObservation(*p,"failed original QBUF publishes nothing");
    fake.result=0;fake.error=EOVERFLOW;fake.memory[0].fill(0x64);queue(*p,0);poll(*p,0x64);
    dequeue(*p,0);fake.memory[0].fill(0x11);std::fill(fake.memory[0].begin()+16,fake.memory[0].begin()+80,0x75);
    Buffer offset(0);offset.plane.data_offset=16;offset.plane.bytesused=80;
    check(p->invokeIoctl(base+0x31e0c,23,VIDIOC_QBUF,&offset.b)==0,"valid public data-offset buffer forwarded");poll(*p,0x75);
}
void boundsAndLoss(){
    auto p=provider();setup(*p);for(unsigned i=0;i<4;++i)map(*p,i);
    for(unsigned i=0;i<3;++i){fake.memory[i].fill(static_cast<std::uint8_t>(0x31+i));queue(*p,i);}
    check(p->stats().accepted==2&&p->stats().dropped==1,"two fixed snapshots bound pending coded input without eviction");
    poll(*p,0x31);poll(*p,0x32);const auto reads=fake.dataReads;queue(*p,2);
    check(fake.dataReads==reads,"slot-full QBUF still transfers buffer ownership to kernel");noObservation(*p,"slot-full item cannot be opportunistically recopied");
    dequeue(*p,2);fake.memory[2].fill(0x43);queue(*p,2);const auto old=poll(*p,0x43);
    dequeue(*p,2);fake.memory[2].fill(0x54);queue(*p,2);
    int type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;fake.result=-1;fake.error=EBUSY;
    check(p->invokeIoctl(base+0x312e8,23,VIDIOC_STREAMOFF,&type)==-1&&errno==EBUSY,"failed STREAMOFF still forwarded exactly");
    noObservation(*p,"even attempted stream reset invalidates old queued evidence");
    fake.result=0;fake.error=EOVERFLOW;setup(*p);map(*p,0);fake.memory[0].fill(0x65);queue(*p,0);const auto fresh=poll(*p,0x65);
    check(fresh.poolGeneration>old.poolGeneration,"new output pool generation monotonically distinct");
    dequeue(*p,0);queue(*p,0);errno=EALREADY;
    check(p->invokeMunmap(0x999,fake.memory[0].data()+1,10)==0&&errno==fake.error&&fake.receivedErrno==EALREADY,"any caller partial unmap invalidates before exact original forwarding");
    noObservation(*p,"partial unmap clears old copied evidence");queue(*p,0);noObservation(*p,"unmapped buffer cannot publish");
    setup(*p);map(*p,0);queue(*p,0);errno=EALREADY;
    check(p->invokeClose(0x999,23)==0&&fake.receivedErrno==EALREADY&&errno==fake.error,"close exact forwarding");
    noObservation(*p,"close invalidates device and all pending evidence");
    setup(*p);map(*p,0);fake.revokeDuringQbuf=p.get();queue(*p,0);noObservation(*p,"late successful QBUF cannot revive stopped generation");
    check(!p->knownIoctl(base+0x31e0c,VIDIOC_QBUF),"stop keeps unknown calls on original tail route");
}
void unsafeShapes(){
    auto p=provider();setup(*p);map(*p,0);Buffer b(0);
    for(unsigned which=0;which<6;++which){
        b.b.memory=V4L2_MEMORY_MMAP;b.b.length=1;b.plane.bytesused=64;b.plane.data_offset=0;b.b.timestamp.tv_usec=0;b.b.flags=V4L2_BUF_FLAG_TIMESTAMP_COPY;
        if(which==0)b.b.memory=V4L2_MEMORY_USERPTR;
        if(which==1)b.b.length=2;
        if(which==2)b.plane.bytesused=4097;
        if(which==3)b.plane.data_offset=65;
        if(which==4)b.b.timestamp.tv_usec=1;
        if(which==5)b.b.flags=V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        const auto reads=fake.dataReads;
        check(p->invokeIoctl(base+0x31e0c,23,VIDIOC_QBUF,&b.b)==0,"unsupported shape preserves original successful return");
        check(fake.dataReads==reads,"unsupported buffer never dereferenced as coded bytes");noObservation(*p,"unsupported shape yields no fact");
        setup(*p);map(*p,0);
    }
    fake.readFails=true;queue(*p,0);noObservation(*p,"failed safe self-read cannot invent metadata");fake.readFails=false;
    setup(*p);Buffer q(0);fake.mapFails=true;p->invokeIoctl(base+0x32fec,23,VIDIOC_QUERYBUF,&q.b);
    check(p->invokeMmap(base+0x33008,nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,23,0)==MAP_FAILED,"failed original map retained");
    fake.mapFails=false;queue(*p,0);noObservation(*p,"failed map grants no mapped ownership");
    check(!nd::preparePinnedProcess(calls()),"actual host executable cannot impersonate pinned vrlink");
    const auto entries=nd::preparedEntries();check(!entries.ioctl&&!entries.mmap&&!entries.close&&!entries.munmap,"unprepared runtime exposes no hook entries");
}
void captureOwnership(){
    auto p=provider();check(p->setCaptureSink([](const nd::CaptureObservation& fact,void*) noexcept{++fake.sinkCalls;fake.lastSink=fact;},nullptr),"source sink registers before actual native pool");
    setup(*p);check(!p->setCaptureSink(nullptr,nullptr),"late sink registration refused after lifecycle already began");
    map(*p,0);map(*p,1);fake.memory[0].fill(0x42);fake.memory[1].fill(0x43);
    auto capture=format();capture.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    check(p->invokeIoctl(base+0x3242c,23,VIDIOC_S_FMT,&capture)==0,"actual capture format forwarded");
    v4l2_requestbuffers request{};request.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;request.memory=V4L2_MEMORY_DMABUF;request.count=4;
    check(p->invokeIoctl(base+0x324a8,23,VIDIOC_REQBUFS,&request)==0,"capture DMABUF pool configured");
    Buffer b(0);b.b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;b.b.memory=V4L2_MEMORY_DMABUF;
    check(p->invokeIoctl(base+0x325c8,23,VIDIOC_QUERYBUF,&b.b)==0,"capture slot query observed");
    nd::CaptureObservation event{};while(p->pollCapture(event)){}
    b.plane.m.fd=73;b.b.timestamp.tv_sec=0x4321;b.b.timestamp.tv_usec=0;fake.requireCaptureSink=true;
    check(p->invokeIoctl(base+0x310f8,23,VIDIOC_QBUF,&b.b)==0,"DMABUF capture queued unchanged");
    check(p->pollCapture(event)&&event.kind==nd::CaptureKind::Requeue&&event.dmabufFd==73&&event.index==0
        &&event.sourcePoolGeneration&&event.capturePoolGeneration&&event.contentGeneration==1,"requeue invalidation precedes decode and names exact DMABUF lease");
    auto claimed=event;claimed.kind=nd::CaptureKind::Decoded;
    check(!p->captureLeaseCurrent(claimed),"queued buffer cannot be authorized by a claimed decoded enum");
    const auto generation=event.capturePoolGeneration;
    fake.dequeue=0;check(p->invokeIoctl(base+0x32a5c,23,VIDIOC_DQBUF,&b.b)==0,"actual capture result original once");
    check(p->pollCapture(event)&&event.kind==nd::CaptureKind::Decoded&&event.dmabufFd==73&&event.nativeTimestampToken==0x4321
        &&event.capturePoolGeneration==generation&&event.contentGeneration==1,"decoded timestamp distinct from capture index and content lease");
    const auto validLease=event;check(p->captureLeaseCurrent(validLease),"immediate scalar check admits only successful actual current DQBUF lease");
    auto wrongLease=validLease;++wrongLease.nativeTimestampToken;
    check(!p->captureLeaseCurrent(wrongLease),"current scalar lease also binds actual timestamp token");
    check(p->invokeIoctl(base+0x32a5c,23,VIDIOC_DQBUF,&b.b)==0&&!p->pollCapture(event),"unqueued duplicate capture gives no evidence");
    queue(*p,0);poll(*p,0x42); // Capture setup must not destroy unrelated OUTPUT mappings.
    fake.result=-1;check(p->invokeIoctl(base+0x310f8,23,VIDIOC_QBUF,&b.b)==-1,"failed capture queue original preserved");
    check(!p->captureLeaseCurrent(validLease),"attempted QBUF invalidates selected old lease before worker event consumption even on failure");
    check(p->pollCapture(event)&&event.kind==nd::CaptureKind::Requeue,"attempted reuse conservatively invalidates old image");
    fake.result=0;p->invokeIoctl(base+0x32a5c,23,VIDIOC_DQBUF,&b.b);
    check(!p->pollCapture(event),"failed queue cannot establish decoded ownership");
    p->invokeIoctl(base+0x3242c,23,VIDIOC_S_FMT,&capture);
    check(p->pollCapture(event)&&event.kind==nd::CaptureKind::Reset&&event.capturePoolGeneration!=generation,"capture reconfiguration retires exact old pool");
    queue(*p,1);poll(*p,0x43);
    p->stop();check(fake.lastSink.kind==nd::CaptureKind::SourceReset,"stop revokes shared source sink synchronously before teardown");
    check(p->setCaptureSink(nullptr,nullptr),"sink can detach only after stopped native ownership");
}
}
int main(){try{identityAndOwnership();boundsAndLoss();unsafeShapes();captureOwnership();std::printf("public V4L2 coded-input provider: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
#endif
