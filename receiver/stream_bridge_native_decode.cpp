#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stream_bridge_native_decode.h"
#include "sha256.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <link.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

namespace stream_bridge_native {
const Hash ExpectedVrlinkHash{{0x31,0xad,0xde,0xf0,0xc9,0x55,0x3d,0x37,0x70,0xb5,0x57,0x04,0x87,0x69,0x9e,0x96,0xd7,0x02,0x71,0xd4,0x07,0x3c,0xd6,0xc0,0x3b,0x08,0x0f,0xde,0xe6,0xae,0x21,0x53}};
const Hash ExpectedExecutableSegmentHash{{0x55,0x64,0x65,0x31,0xfe,0x10,0xd7,0x87,0x87,0x72,0x6b,0xa6,0xdd,0x68,0xe0,0x61,0xbc,0x16,0x28,0x31,0xa0,0x83,0xc3,0x43,0x0c,0x03,0x20,0xf7,0xb3,0x3c,0x62,0xd9}};
namespace {
static_assert(sizeof(void*)==8&&sizeof(v4l2_buffer)==88&&sizeof(v4l2_plane)==64,"public LP64 V4L2 ABI");
static_assert(VIDIOC_QBUF==0xc058560fUL&&VIDIOC_QUERYBUF==0xc0585609UL&&VIDIOC_DQBUF==0xc0585611UL,"retained public request ABI");
struct Call{std::uintptr_t offset;unsigned long request;};
constexpr Call Calls[]={{0x32edc,VIDIOC_S_FMT},{0x32f18,VIDIOC_REQBUFS},{0x32fec,VIDIOC_QUERYBUF},
    {0x31e0c,VIDIOC_QBUF},{0x32bc0,VIDIOC_DQBUF},{0x31368,VIDIOC_STREAMON},
    {0x312e8,VIDIOC_STREAMOFF},{0x315dc,VIDIOC_REQBUFS},
    {0x3242c,VIDIOC_S_FMT},{0x324a8,VIDIOC_REQBUFS},{0x325c8,VIDIOC_QUERYBUF},
    {0x310f8,VIDIOC_QBUF},{0x32a5c,VIDIOC_DQBUF}};
void increment(std::uint64_t& n) noexcept{if(n!=std::numeric_limits<std::uint64_t>::max())++n;}
bool overlap(std::uintptr_t a,std::size_t n,std::uintptr_t b,std::size_t m) noexcept {
    if(!n||!m)return false;
    return a<=b?b-a<n:a-b<m;
}
bool ordinaryBuffer(const v4l2_buffer& b) noexcept {
    return b.type==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE&&b.memory==V4L2_MEMORY_MMAP
        &&b.length==1&&b.m.planes&&b.index<MaxBuffers;
}
}
struct DecodeProvider::Impl{
    struct Buffer{bool queried=false,mapped=false,queued=false,pending=false;std::uint32_t offset=0,length=0;std::uintptr_t address=0;};
    struct CaptureBuffer{bool queried=false,queued=false,decoded=false;int dmabuf=-1;std::uint64_t cycle=0;std::uint32_t token=0;};
    struct Device{std::uint64_t captureGeneration=0;std::uint32_t captureCount=0;bool captureConfigured=false;std::array<CaptureBuffer,MaxBuffers> capture{};int fd=-1;bool hevc=false;std::uint32_t count=0;std::uint64_t generation=0,content=0;std::array<Buffer,MaxBuffers> buffers{};};
    struct Slot{enum State{Free,Reserved,Ready}state=Free;std::uint64_t ticket=0;Observation fact{};std::array<std::uint8_t,MaxCodedBytes> bytes{};};
    mutable std::mutex mutex;std::uintptr_t base=0;Originals calls{};Reader reader{};
    bool prepared=false,active=false;std::uint64_t generation=0,ticket=0;
    CaptureSink sink=nullptr;void* sinkContext=nullptr;
    std::array<CaptureObservation,64> captureEvents{};std::size_t captureHead=0,captureSize=0;
    std::array<Device,MaxDevices> devices{};std::array<Slot,SnapshotCapacity> slots{};SnapshotStats counts{};
    Device* find(int fd){for(auto& d:devices)if(d.fd==fd&&fd>=0)return &d;return nullptr;}
    Device* allocate(int fd){if(auto* d=find(fd))return d;for(auto& d:devices)if(d.fd<0){d.fd=fd;return &d;}return nullptr;}
    void captureEvent(const CaptureObservation& event){
        if(sink&&event.kind!=CaptureKind::InputGap)sink(event,sinkContext);
        if(captureSize==captureEvents.size()){increment(counts.captureDropped);return;}
        captureEvents[(captureHead+captureSize)%captureEvents.size()]=event;++captureSize;
    }
    void revokeCapture(Device& d,bool source=false){
        d.capture={};d.captureCount=0;d.captureConfigured=false;
        if(generation==~std::uint64_t(0)){d.captureGeneration=0;active=false;}else d.captureGeneration=++generation;
        captureEvent({source?CaptureKind::SourceReset:CaptureKind::Reset,d.fd,-1,0,0,d.generation,d.captureGeneration,0});
    }
    void revoke(Device& d){
        for(auto& s:slots)if(s.state!=Slot::Free&&s.fact.fd==d.fd)s.state=Slot::Free;
        revokeCapture(d,true);d.buffers={};d.count=0;d.content=0;
        if(generation==~std::uint64_t(0)){d.generation=0;active=false;}else d.generation=++generation;
    }
    bool captureCurrent(const CaptureObservation& fact){
        if(!active||fact.kind!=CaptureKind::Decoded||fact.fd<0||fact.dmabufFd<0||!fact.sourcePoolGeneration
            ||!fact.capturePoolGeneration||!fact.contentGeneration||fact.index>=MaxBuffers)return false;
        const auto* d=find(fact.fd);if(!d||!d->hevc||d->generation!=fact.sourcePoolGeneration
            ||d->captureGeneration!=fact.capturePoolGeneration||fact.index>=d->captureCount)return false;
        const auto& buffer=d->capture[fact.index];return buffer.queried&&buffer.decoded&&!buffer.queued
            &&buffer.dmabuf==fact.dmabufFd&&buffer.cycle==fact.contentGeneration&&buffer.token==fact.nativeTimestampToken;
    }
    bool read(void* out,const void* source,std::size_t n){return reader.copy&&source&&reader.copy(out,reinterpret_cast<std::uintptr_t>(source),n,reader.context);}
    bool shape(void* source,v4l2_buffer& b,v4l2_plane& plane){return read(&b,source,sizeof b)&&ordinaryBuffer(b)&&read(&plane,b.m.planes,sizeof plane);}
};
DecodeProvider::DecodeProvider():impl_(new Impl){}
DecodeProvider::~DecodeProvider()=default;
bool DecodeProvider::prepare(const ImageIdentity& image,const Originals& originals,Reader reader) noexcept{
    if(!image.base||image.file!=ExpectedVrlinkHash||image.executableSegment!=ExpectedExecutableSegmentHash
        ||!originals.ioctl||!originals.mmap||!originals.munmap||!originals.close||!reader.copy)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->prepared&&(impl_->base!=image.base||impl_->calls.ioctl!=originals.ioctl||impl_->calls.mmap!=originals.mmap
        ||impl_->calls.munmap!=originals.munmap||impl_->calls.close!=originals.close))return false;
    impl_->base=image.base;impl_->calls=originals;impl_->reader=reader;impl_->prepared=impl_->active=true;return true;
}
void DecodeProvider::stop() noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->active=false;
    for(auto& device:impl_->devices)if(device.fd>=0)impl_->revokeCapture(device,true);
    impl_->devices={};
    for(auto& s:impl_->slots){s.state=Impl::Slot::Free;}
    impl_->captureEvents={};impl_->captureHead=impl_->captureSize=0;
}
OriginalIoctl DecodeProvider::originalIoctl()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->calls.ioctl;}
std::uintptr_t DecodeProvider::imageBase()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->prepared?impl_->base:0;}
bool DecodeProvider::knownIoctl(std::uintptr_t address,unsigned long request)const noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->active||address<impl_->base)return false;
    for(const auto& c:Calls)if(address-impl_->base==c.offset&&request==c.request)return true;
    return false;
}
int DecodeProvider::invokeIoctl(std::uintptr_t caller,int fd,unsigned long request,void* argument) noexcept{
    const int incoming=errno;const auto original=originalIoctl();
    if(!original){errno=ENOSYS;return -1;}
    const bool admitted=knownIoctl(caller,request);
    v4l2_format format{};v4l2_requestbuffers requested{};v4l2_buffer buffer{};v4l2_plane plane{};
    int streamType=0;std::uint64_t expectedGeneration=0,reservation=0;std::size_t slotIndex=SnapshotCapacity;
    InputLoss inputLoss=InputLoss::None;bool knownGap=false;
    bool shape=false;const bool captureSite=caller==impl_->base+0x3242c||caller==impl_->base+0x324a8
        ||caller==impl_->base+0x325c8||caller==impl_->base+0x310f8||caller==impl_->base+0x32a5c;
    std::uint64_t captureGeneration=0;v4l2_buffer captureBuffer{};v4l2_plane capturePlane{};bool captureShape=false;
    if(admitted){
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto* d=impl_->find(fd);
        if(request==VIDIOC_S_FMT){shape=impl_->read(&format,argument,sizeof format);
            if(d&&shape&&format.type==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE){impl_->revoke(*d);d->hevc=false;} // revoke before any codec change
        }else if(request==VIDIOC_REQBUFS){shape=impl_->read(&requested,argument,sizeof requested);
            if(d&&shape&&requested.type==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)impl_->revoke(*d);
        }else if(request==VIDIOC_STREAMOFF||request==VIDIOC_STREAMON){shape=impl_->read(&streamType,argument,sizeof streamType);
            if(d&&shape&&request==VIDIOC_STREAMOFF&&streamType==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)impl_->revoke(*d);
        }else shape=impl_->shape(argument,buffer,plane);
        if(d&&d->hevc){
            if(captureSite&&(request==VIDIOC_S_FMT||request==VIDIOC_REQBUFS))impl_->revokeCapture(*d);
            captureGeneration=d->captureGeneration;
            if(captureSite&&request!=VIDIOC_S_FMT&&request!=VIDIOC_REQBUFS){
                captureShape=impl_->read(&captureBuffer,argument,sizeof captureBuffer)&&captureBuffer.type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                    &&captureBuffer.memory==V4L2_MEMORY_DMABUF&&captureBuffer.length==1&&captureBuffer.index<MaxBuffers
                    &&captureBuffer.m.planes&&impl_->read(&capturePlane,captureBuffer.m.planes,sizeof capturePlane);
                if(captureShape&&request==VIDIOC_QBUF&&d->captureConfigured&&captureBuffer.index<d->captureCount){
                    auto& b=d->capture[captureBuffer.index];
                    b.decoded=false;
                    if(b.queried&&!b.queued&&capturePlane.m.fd>=0&&capturePlane.length&&b.cycle!=~std::uint64_t(0)){
                        b.dmabuf=capturePlane.m.fd;b.decoded=false;b.token=0;++b.cycle;
                        impl_->captureEvent({CaptureKind::Requeue,fd,b.dmabuf,captureBuffer.index,0,d->generation,d->captureGeneration,b.cycle});
                    }else captureShape=false;
                }
                if(request==VIDIOC_QBUF&&!captureShape)impl_->revokeCapture(*d);
            }
        }
        if(d)expectedGeneration=d->generation;
        if(request==VIDIOC_QBUF&&d&&d->hevc&&d->generation&&shape&&buffer.index<d->count){
            auto& b=d->buffers[buffer.index];
            const bool valid=b.queried&&b.mapped&&!b.queued&&!b.pending&&plane.length==b.length
                &&plane.data_offset<=plane.bytesused&&plane.bytesused<=b.length&&plane.bytesused>plane.data_offset
                &&plane.bytesused-plane.data_offset<=MaxCodedBytes&&buffer.timestamp.tv_sec>=0
                &&std::uint64_t(buffer.timestamp.tv_sec)<=std::numeric_limits<std::uint32_t>::max()
                &&buffer.timestamp.tv_usec==0&&(buffer.flags&V4L2_BUF_FLAG_TIMESTAMP_MASK)==V4L2_BUF_FLAG_TIMESTAMP_COPY;
            if(valid){
                knownGap=true;
                for(std::size_t i=0;i<impl_->slots.size();++i)if(impl_->slots[i].state==Impl::Slot::Free){slotIndex=i;break;}
                if(slotIndex<SnapshotCapacity&&impl_->ticket!=~std::uint64_t(0)){
                    auto& slot=impl_->slots[slotIndex];slot.state=Impl::Slot::Reserved;reservation=slot.ticket=++impl_->ticket;
                    const auto used=plane.bytesused-plane.data_offset;
                    if(impl_->reader.copy(slot.bytes.data(),b.address+plane.data_offset,used,impl_->reader.context)){
                        b.pending=true;slot.fact={fd,buffer.index,used,d->generation,0,static_cast<std::uint32_t>(buffer.timestamp.tv_sec),false,b.length};
                    }else{slot.state=Impl::Slot::Free;slotIndex=SnapshotCapacity;inputLoss=InputLoss::CopyRejected;}
                }else inputLoss=InputLoss::SnapshotFull;
            }else inputLoss=InputLoss::CopyRejected;
        }
    }
    errno=incoming;const int result=original(fd,request,argument);const int error=errno;
    if(admitted){
        std::lock_guard<std::mutex> lock(impl_->mutex);auto* d=impl_->find(fd);
        if(result==0&&impl_->active&&d&&d->hevc&&d->captureGeneration==captureGeneration){
            if(captureSite&&request==VIDIOC_S_FMT){
                v4l2_format actual{};
                if(impl_->read(&actual,argument,sizeof actual)&&actual.type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE&&actual.fmt.pix_mp.num_planes==1)
                    d->captureConfigured=true;
            }else if(captureSite&&request==VIDIOC_REQBUFS){
                v4l2_requestbuffers actual{};
                if(impl_->read(&actual,argument,sizeof actual)&&actual.type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                    &&actual.memory==V4L2_MEMORY_DMABUF&&actual.count<=MaxBuffers){d->captureConfigured=true;d->captureCount=actual.count;}
            }else if(captureSite&&request==VIDIOC_QUERYBUF&&captureShape&&captureBuffer.index<d->captureCount){
                v4l2_buffer actual{};v4l2_plane actualPlane{};
                if(impl_->read(&actual,argument,sizeof actual)&&actual.type==captureBuffer.type&&actual.memory==captureBuffer.memory
                    &&actual.length==1&&actual.index==captureBuffer.index&&actual.m.planes
                    &&impl_->read(&actualPlane,actual.m.planes,sizeof actualPlane))d->capture[actual.index].queried=true;
            }else if(captureSite&&request==VIDIOC_QBUF&&captureShape&&captureBuffer.index<d->captureCount){
                auto& b=d->capture[captureBuffer.index];if(b.cycle&&b.dmabuf==capturePlane.m.fd)b.queued=true;
            }else if(captureSite&&request==VIDIOC_DQBUF){
                v4l2_buffer actual{};v4l2_plane actualPlane{};
                const bool valid=impl_->read(&actual,argument,sizeof actual)&&actual.type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                    &&actual.memory==V4L2_MEMORY_DMABUF&&actual.length==1&&actual.index<d->captureCount;
                if(valid){auto& b=d->capture[actual.index];
                    const bool copied=actual.m.planes&&impl_->read(&actualPlane,actual.m.planes,sizeof actualPlane);
                    b.decoded=false;
                    if(b.queried&&b.queued&&b.cycle&&copied&&actual.timestamp.tv_sec>=0&&actual.timestamp.tv_sec<=0xffff
                        &&actual.timestamp.tv_usec==0&&!(actual.flags&V4L2_BUF_FLAG_ERROR)){
                        b.decoded=true;b.token=static_cast<std::uint32_t>(actual.timestamp.tv_sec);
                        impl_->captureEvent({CaptureKind::Decoded,fd,b.dmabuf,actual.index,static_cast<std::uint32_t>(actual.timestamp.tv_sec),d->generation,d->captureGeneration,b.cycle});
                    }
                    b.queued=false;
                }
            }
        }
        if(result==0&&impl_->active){
            if(request==VIDIOC_S_FMT&&shape&&format.type==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                &&format.fmt.pix_mp.pixelformat==V4L2_PIX_FMT_HEVC){
                v4l2_format actual{};
                if(impl_->read(&actual,argument,sizeof actual)&&actual.type==format.type
                    &&actual.fmt.pix_mp.pixelformat==V4L2_PIX_FMT_HEVC&&actual.fmt.pix_mp.num_planes==1){
                    d=impl_->allocate(fd);if(d){impl_->revoke(*d);d->hevc=d->generation!=0;}
                }
            }else if(d&&d->hevc&&d->generation==expectedGeneration&&request==VIDIOC_REQBUFS&&shape
                &&requested.type==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE&&requested.memory==V4L2_MEMORY_MMAP){
                v4l2_requestbuffers actual{};
                if(impl_->read(&actual,argument,sizeof actual)&&actual.type==requested.type&&actual.memory==requested.memory&&actual.count<=MaxBuffers)d->count=actual.count;
            }else if(d&&d->hevc&&d->generation==expectedGeneration&&request==VIDIOC_QUERYBUF){
                v4l2_buffer actual{};v4l2_plane p{};
                if(impl_->shape(argument,actual,p)&&actual.index<d->count&&p.length&&p.length<=16*1024*1024
                    &&!(p.m.mem_offset&4095)){auto& b=d->buffers[actual.index];if(!b.mapped&&!b.queued&&!b.pending){b.queried=true;b.length=p.length;b.offset=p.m.mem_offset;}}
            }else if(d&&d->generation==expectedGeneration&&request==VIDIOC_DQBUF){
                v4l2_buffer actual{};v4l2_plane p{};
                if(impl_->shape(argument,actual,p)&&actual.index<d->count){auto& b=d->buffers[actual.index];if(b.queued)b.queued=false;}
            }
        }
        if(impl_->active&&result==0&&d&&d->generation==expectedGeneration&&request==VIDIOC_QBUF&&!captureSite){
            if(shape&&buffer.index<d->count){
                d->buffers[buffer.index].queued=true; // Native ownership transferred even without a snapshot.
                if(inputLoss!=InputLoss::None){
                    auto& sequence=inputLoss==InputLoss::SnapshotFull?impl_->counts.dropped:impl_->counts.rejected;
                    // Publish the loss and its exact gap together, only AFTER a
                    // successful original QBUF on this still-current pool.
                    increment(sequence);
                    if(knownGap)impl_->captureEvent({CaptureKind::InputGap,fd,-1,buffer.index,
                        static_cast<std::uint32_t>(buffer.timestamp.tv_sec),d->generation,0,0,inputLoss,sequence});
                }
            }else impl_->revoke(*d); // Successful unsupported shape loses ownership evidence.
        }
        if(slotIndex<SnapshotCapacity){auto& slot=impl_->slots[slotIndex];
            if(slot.state==Impl::Slot::Reserved&&slot.ticket==reservation){
                if(impl_->active&&result==0&&d&&d->generation==expectedGeneration&&buffer.index<d->count){
                    auto& b=d->buffers[buffer.index];b.pending=false;b.queued=true;
                    if(d->content!=~std::uint64_t(0)){slot.fact.contentGeneration=++d->content;slot.state=Impl::Slot::Ready;increment(impl_->counts.accepted);}
                    else slot.state=Impl::Slot::Free;
                }else{slot.state=Impl::Slot::Free;if(d&&d->generation==expectedGeneration&&buffer.index<d->count)d->buffers[buffer.index].pending=false;}
            }
        }
    }
    errno=error;return result;
}
void* DecodeProvider::invokeMmap(std::uintptr_t caller,void* address,std::size_t size,int prot,int flags,int fd,off_t offset) noexcept{
    const int incoming=errno;OriginalMmap original;std::uint64_t generation=0;
    {std::lock_guard<std::mutex> lock(impl_->mutex);original=impl_->calls.mmap;
        if(impl_->active&&caller==impl_->base+0x33008)if(auto* d=impl_->find(fd))generation=d->generation;}
    if(!original){errno=ENOSYS;return MAP_FAILED;}
    errno=incoming;void* result=original(address,size,prot,flags,fd,offset);const int error=errno;
    if(result!=MAP_FAILED&&generation&&address==nullptr&&prot==(PROT_READ|PROT_WRITE)&&flags==MAP_SHARED&&offset>=0){
        std::lock_guard<std::mutex> lock(impl_->mutex);auto* d=impl_->find(fd);
        if(impl_->active&&d&&d->generation==generation&&size<=~std::uintptr_t(0)-reinterpret_cast<std::uintptr_t>(result)){
            Impl::Buffer* candidate=nullptr;unsigned matches=0;
            for(auto& b:d->buffers)if(b.queried&&b.length==size&&b.offset==std::uint64_t(offset)){candidate=&b;++matches;}
            if(matches==1&&candidate&&!candidate->mapped){auto& b=*candidate;
            bool ambiguous=false;for(const auto& device:impl_->devices)for(const auto& other:device.buffers)
                if(other.mapped&&overlap(other.address,other.length,reinterpret_cast<std::uintptr_t>(result),size))ambiguous=true;
            if(!ambiguous){b.mapped=true;b.address=reinterpret_cast<std::uintptr_t>(result);}
            }
        }
    }
    errno=error;return result;
}
int DecodeProvider::invokeMunmap(std::uintptr_t,void* address,std::size_t size) noexcept{
    OriginalMunmap original;const int incoming=errno;
    {std::lock_guard<std::mutex> lock(impl_->mutex);original=impl_->calls.munmap;
        // Any unmap caller can invalidate our mapping; revoke BEFORE forwarding.
        for(auto& d:impl_->devices)for(auto& b:d.buffers)if(b.mapped&&overlap(b.address,b.length,reinterpret_cast<std::uintptr_t>(address),size))impl_->revoke(d);}
    if(!original){errno=ENOSYS;return -1;}errno=incoming;return original(address,size);
}
int DecodeProvider::invokeClose(std::uintptr_t,int fd) noexcept{
    OriginalClose original;const int incoming=errno;
    {std::lock_guard<std::mutex> lock(impl_->mutex);original=impl_->calls.close;if(auto* d=impl_->find(fd)){impl_->revoke(*d);*d=Impl::Device{};}
        for(auto& d:impl_->devices)if(d.fd>=0)for(const auto& b:d.capture)if(b.dmabuf==fd){impl_->revokeCapture(d);break;}}
    if(!original){errno=ENOSYS;return -1;}errno=incoming;return original(fd);
}
bool DecodeProvider::poll(Observation& observation,std::uint8_t* output,std::size_t capacity) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Slot* first=nullptr;
    for(auto& slot:impl_->slots)if(slot.state==Impl::Slot::Ready&&(!first||slot.ticket<first->ticket))first=&slot;
    if(!first||!output||capacity<first->fact.usedBytes)return false;
    std::copy_n(first->bytes.data(),first->fact.usedBytes,output);observation=first->fact;first->state=Impl::Slot::Free;return true;
}
bool DecodeProvider::pollCapture(CaptureObservation& out) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->captureSize)return false;
    out=impl_->captureEvents[impl_->captureHead];impl_->captureHead=(impl_->captureHead+1)%impl_->captureEvents.size();--impl_->captureSize;return true;
}
bool DecodeProvider::captureLeaseCurrent(const CaptureObservation& fact)const noexcept{
    std::unique_lock<std::mutex> lock(impl_->mutex,std::try_to_lock);
    return lock&&impl_->captureCurrent(fact);
}
bool DecodeProvider::captureLeaseCurrentOnWorker(const CaptureObservation& fact)const noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->captureCurrent(fact);
}
bool DecodeProvider::setCaptureSink(CaptureSink sink,void* context) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for(const auto& device:impl_->devices)if(device.fd>=0)return false;
    impl_->sink=sink;impl_->sinkContext=context;return true;
}
SnapshotStats DecodeProvider::stats()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->counts;}

namespace {
DecodeProvider* runtime=nullptr;
bool selfRead(void* output,std::uintptr_t source,std::size_t n,void*) noexcept{
    iovec local{output,n},remote{reinterpret_cast<void*>(source),n};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==static_cast<ssize_t>(n);
}
int findMain(dl_phdr_info* info,std::size_t,void* context){
    if(info->dlpi_name&&*info->dlpi_name)return 0;
    auto& image=*static_cast<ImageIdentity*>(context);
    for(unsigned i=0;i<info->dlpi_phnum;++i){const auto& p=info->dlpi_phdr[i];
        if(p.p_type==PT_LOAD&&p.p_flags==(PF_R|PF_X)&&p.p_offset==0&&p.p_vaddr==0&&p.p_filesz==0x247541&&p.p_memsz==p.p_filesz){
            image.base=info->dlpi_addr;frame_receiver::Sha256 sha;sha.update(reinterpret_cast<const void*>(image.base),p.p_filesz);image.executableSegment=sha.finish();return 1;}}
    return 1;
}
int knownEntry(int fd,unsigned long request,void* argument) noexcept{
    return runtime->invokeIoctl(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),fd,request,argument);
}
[[maybe_unused]] void* mmapEntry(void* a,std::size_t n,int prot,int flags,int fd,off_t offset) noexcept{
    return runtime->invokeMmap(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),a,n,prot,flags,fd,offset);
}
[[maybe_unused]] int munmapEntry(void* a,std::size_t n) noexcept{return runtime->invokeMunmap(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),a,n);}
[[maybe_unused]] int closeEntry(int fd) noexcept{return runtime->invokeClose(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),fd);}
template<class To,class From>To bits(From value){static_assert(sizeof(To)==sizeof(From),"LP64 code pointers");To out{};std::memcpy(&out,&value,sizeof out);return out;}
}
extern "C" void* sfnp_native_decode_classify(int,unsigned long request,void*,std::uintptr_t caller) noexcept{
    const int incoming=errno;void* target=runtime->knownIoctl(caller,request)?bits<void*>(&knownEntry):bits<void*>(runtime->originalIoctl());errno=incoming;return target;
}
#if defined(__aarch64__)
extern "C" void sfnp_stream_decode_ioctl_entry();
#endif
bool preparePinnedProcess(const Originals& originals) noexcept{
    if(runtime)return false; // Immutable published originals for process lifetime.
    ImageIdentity image{};auto* file=std::fopen("/proc/self/exe","rb");if(!file)return false;
    frame_receiver::Sha256 sha;std::array<std::uint8_t,65536> block{};std::size_t total=0,n=0;
    while((n=std::fread(block.data(),1,block.size(),file))!=0){total+=n;if(total>2425024)break;sha.update(block.data(),n);}
    const bool readOkay=!std::ferror(file)&&total==2425024;std::fclose(file);if(!readOkay)return false;
    image.file=sha.finish();if(image.file!=ExpectedVrlinkHash)return false;
    dl_iterate_phdr(findMain,&image);if(image.executableSegment!=ExpectedExecutableSegmentHash)return false;
    try{auto candidate=std::make_unique<DecodeProvider>();if(!candidate->prepare(image,originals,{selfRead,nullptr}))return false;runtime=candidate.release();return true;}catch(...){return false;}
}
void stopPinnedProcess() noexcept{if(runtime)runtime->stop();}
bool pollPinnedProcess(Observation& fact,std::uint8_t* output,std::size_t size) noexcept{return runtime&&runtime->poll(fact,output,size);}
bool pollPinnedCapture(CaptureObservation& fact) noexcept{return runtime&&runtime->pollCapture(fact);}
SnapshotStats pinnedSnapshotStats() noexcept{return runtime?runtime->stats():SnapshotStats{};}
bool pinnedCaptureLeaseCurrent(const CaptureObservation& fact) noexcept{return runtime&&runtime->captureLeaseCurrent(fact);}
bool pinnedCaptureLeaseCurrentOnWorker(const CaptureObservation& fact) noexcept{return runtime&&runtime->captureLeaseCurrentOnWorker(fact);}
bool setPinnedCaptureSink(CaptureSink sink,void* context) noexcept{return runtime&&runtime->setCaptureSink(sink,context);}
std::uintptr_t preparedImageBase() noexcept{return runtime?runtime->imageBase():0;}
Entries preparedEntries() noexcept{
#if defined(__aarch64__)
    if(runtime)return {bits<void*>(&sfnp_stream_decode_ioctl_entry),mmapEntry,munmapEntry,closeEntry};
#endif
    return {};
}
} // namespace stream_bridge_native
