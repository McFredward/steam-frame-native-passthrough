#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stream_bridge_local.h"
#include "sha256.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <new>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace stream_bridge_local {
namespace {
namespace nd=stream_bridge_native;
constexpr std::size_t SharedBytes=4096,SlotCount=nd::MaxDevices*nd::MaxBuffers,HeaderBytes=192;
constexpr std::uint64_t Magic=0x31534c504e4653ULL;
struct Slot {std::atomic<std::uint64_t> sequence{0};std::array<std::atomic<std::uint64_t>,10> fields{};};
struct Shared {std::atomic<std::uint64_t> magic{Magic},active{1};std::array<Slot,SlotCount> slots{};};
static_assert(sizeof(Shared)<=SharedBytes&&std::atomic<std::uint64_t>::is_always_lock_free,"fixed lock-free cross-process lease ABI");
using Words=std::array<std::uint64_t,10>;
Words words(const nd::CaptureObservation& c,KernelIdentity kernel){return {{static_cast<std::uint64_t>(c.fd),c.index,c.nativeTimestampToken,c.sourcePoolGeneration,c.capturePoolGeneration,c.contentGeneration,kernel[0],kernel[1],static_cast<std::uint64_t>(c.dmabufFd),1}};}
bool identity(int fd,KernelIdentity& out){struct stat st{};if(fd<0||fstat(fd,&st)||!st.st_ino)return false;out={{static_cast<std::uint64_t>(st.st_dev),static_cast<std::uint64_t>(st.st_ino)}};return true;}
bool snapshot(const Shared* shared,std::uint32_t index,std::uint64_t epoch,std::uint64_t& sequence,Words& out){
    if(!shared||!epoch||index>=SlotCount||shared->magic.load(std::memory_order_acquire)!=Magic||shared->active.load(std::memory_order_acquire)!=epoch)return false;
    const auto& slot=shared->slots[index];const auto before=slot.sequence.load(std::memory_order_acquire);if(!before||(before&1))return false;
    for(std::size_t i=0;i<out.size();++i)out[i]=slot.fields[i].load(std::memory_order_acquire);
    if(slot.sequence.load(std::memory_order_acquire)!=before||shared->active.load(std::memory_order_acquire)!=epoch||out[9]!=1)return false;
    sequence=before;return true;
}
void store(Slot& slot,const Words& values){const auto sequence=slot.sequence.load(std::memory_order_relaxed);if(sequence>~std::uint64_t(0)-2){slot.sequence.store(~std::uint64_t(0),std::memory_order_release);return;}
    slot.sequence.store(sequence+1,std::memory_order_release);for(std::size_t i=0;i<values.size();++i)slot.fields[i].store(values[i],std::memory_order_release);
    slot.sequence.store(sequence+2,std::memory_order_release);}
bool nonzero(const stream_bridge::Nonce& n){return std::any_of(n.begin(),n.end(),[](auto b){return b!=0;});}
void put(std::uint8_t* p,std::uint64_t n,unsigned count){for(unsigned i=0;i<count;++i)p[i]=static_cast<std::uint8_t>(n>>(8*i));}
std::uint64_t get(const std::uint8_t* p,unsigned count){std::uint64_t n=0;for(unsigned i=0;i<count;++i)n|=std::uint64_t(p[i])<<(8*i);return n;}
void closeFd(int& fd){if(fd>=0)close(fd);fd=-1;}
bool pending(){return errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK||errno==EINPROGRESS||errno==EALREADY;}
int duplicate(int fd){return fd>=0?fcntl(fd,F_DUPFD_CLOEXEC,0):-1;}
}
struct LeaseWriter::Impl {
    struct Resource {std::atomic<int> fd{-1};std::atomic<std::uint64_t> sequence{0},device{0},inode{0};};
    struct NativeSlot {int fd=-1;std::uint32_t index=0;nd::CaptureObservation last{};};
    mutable std::mutex mutex;Shared* shared=nullptr;int fd=-1,readonly=-1;
    std::array<Resource,64> resources{};std::array<NativeSlot,SlotCount> native{};
    void revoke(){if(shared)shared->active.store(0,std::memory_order_release);}
};
LeaseWriter::LeaseWriter():impl_(new Impl){}
LeaseWriter::~LeaseWriter(){revoke();if(impl_->shared)munmap(impl_->shared,SharedBytes);closeFd(impl_->readonly);closeFd(impl_->fd);}
bool LeaseWriter::prepareCold() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->shared)return false;
    impl_->fd=memfd_create("sfnp-capture-leases-v1",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(impl_->fd<0||ftruncate(impl_->fd,SharedBytes)){closeFd(impl_->fd);return false;}
    auto* map=mmap(nullptr,SharedBytes,PROT_READ|PROT_WRITE,MAP_SHARED,impl_->fd,0);if(map==MAP_FAILED){closeFd(impl_->fd);return false;}
    impl_->shared=new(map)Shared();
    if(fcntl(impl_->fd,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)){impl_->revoke();return false;}
    char path[64];std::snprintf(path,sizeof path,"/proc/self/fd/%d",impl_->fd);impl_->readonly=open(path,O_RDONLY|O_CLOEXEC);
    if(impl_->readonly<0){impl_->revoke();return false;}return true;
}
bool LeaseWriter::associateResourceFd(int fd) noexcept {
    KernelIdentity kernel{};if(!identity(fd,kernel))return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->shared||!impl_->shared->active.load(std::memory_order_acquire))return false;
    Impl::Resource* target=nullptr;
    for(auto& r:impl_->resources)if(r.fd.load(std::memory_order_acquire)==fd){target=&r;break;}
    if(!target)for(auto& r:impl_->resources)if(r.fd.load(std::memory_order_acquire)<0){target=&r;break;}
    if(target){const auto sequence=target->sequence.load(std::memory_order_relaxed);if(sequence>~std::uint64_t(0)-2){impl_->revoke();return false;}
        target->sequence.store(sequence+1,std::memory_order_release);target->device.store(kernel[0],std::memory_order_release);target->inode.store(kernel[1],std::memory_order_release);
        target->fd.store(fd,std::memory_order_release);target->sequence.store(sequence+2,std::memory_order_release);return true;}
    impl_->revoke();return false;
}
void LeaseWriter::captureEvent(const nd::CaptureObservation& event) noexcept {
    if(!impl_->shared||!impl_->shared->active.load(std::memory_order_acquire))return;
    if(event.kind==nd::CaptureKind::Reset||event.kind==nd::CaptureKind::SourceReset){
        for(std::size_t i=0;i<impl_->native.size();++i)if(impl_->native[i].fd==event.fd){store(impl_->shared->slots[i],{});impl_->native[i]={};}
        return;
    }
    if(event.fd<0||event.index>=nd::MaxBuffers){impl_->revoke();return;}
    std::size_t index=SlotCount;
    for(std::size_t i=0;i<impl_->native.size();++i)if(impl_->native[i].fd==event.fd&&impl_->native[i].index==event.index){index=i;break;}
    if(index==SlotCount)for(std::size_t i=0;i<impl_->native.size();++i)if(impl_->native[i].fd<0){index=i;break;}
    if(index==SlotCount){impl_->revoke();return;}
    auto& native=impl_->native[index];native.fd=event.fd;native.index=event.index;native.last=event;
    if(event.kind!=nd::CaptureKind::Decoded){store(impl_->shared->slots[index],{});return;}
    KernelIdentity kernel{};
    for(const auto& resource:impl_->resources){const auto sequence=resource.sequence.load(std::memory_order_acquire);if(!sequence||(sequence&1))continue;
        if(resource.fd.load(std::memory_order_acquire)==event.dmabufFd){const KernelIdentity candidate{{resource.device.load(std::memory_order_acquire),resource.inode.load(std::memory_order_acquire)}};
            if(resource.sequence.load(std::memory_order_acquire)==sequence){kernel=candidate;break;}}
    }
    if(!kernel[1]||!event.sourcePoolGeneration||!event.capturePoolGeneration||!event.contentGeneration||event.nativeTimestampToken>0xffff){store(impl_->shared->slots[index],{});return;}
    store(impl_->shared->slots[index],words(event,kernel));
}
void LeaseWriter::sink(const nd::CaptureObservation& event,void* context) noexcept {if(context)static_cast<LeaseWriter*>(context)->captureEvent(event);}
bool LeaseWriter::reference(const nd::CaptureObservation& expected,LeaseReference& output) const noexcept {
    if(expected.kind!=nd::CaptureKind::Decoded)return false;
    if(!impl_->shared)return false;
    const auto epoch=impl_->shared->active.load(std::memory_order_acquire);if(!epoch)return false;
    LeaseReference result{};unsigned count=0;
    for(std::size_t i=0;i<SlotCount;++i){Words values{};std::uint64_t sequence=0;if(!snapshot(impl_->shared,static_cast<std::uint32_t>(i),epoch,sequence,values))continue;
        const KernelIdentity kernel{{values[6],values[7]}};if(values==words(expected,kernel)){result={static_cast<std::uint32_t>(i),sequence,epoch,kernel,expected};++count;}}
    if(count!=1||impl_->shared->active.load(std::memory_order_acquire)!=epoch)return false;
    output=result;return true;
}
int LeaseWriter::duplicateReadOnly() const noexcept {return impl_->shared&&impl_->shared->active.load(std::memory_order_acquire)?duplicate(impl_->readonly):-1;}
bool LeaseWriter::invalidateCurrentEpoch() noexcept {
    if(!impl_->shared)return false;
    auto& active=impl_->shared->active;auto epoch=active.load(std::memory_order_acquire);
    for(unsigned attempt=0;attempt<8;++attempt){
        if(!epoch)return false;
        const auto next=epoch==~std::uint64_t(0)?0:epoch+1;
        if(active.compare_exchange_strong(epoch,next,std::memory_order_acq_rel,std::memory_order_acquire))return next!=0;
    }
    // Contention/exhaustion can only close the route, never wrap or resurrect it.
    active.store(0,std::memory_order_release);return false;
}
void LeaseWriter::revoke() noexcept {impl_->revoke();}
struct LeaseReader::Impl {const Shared* shared=nullptr;std::uint64_t epoch=0;};
LeaseReader::LeaseReader():impl_(new Impl){}LeaseReader::~LeaseReader(){reset();}
void LeaseReader::reset() noexcept {if(impl_->shared)munmap(const_cast<Shared*>(impl_->shared),SharedBytes);impl_->shared=nullptr;impl_->epoch=0;}
bool LeaseReader::mapReadOnly(int fd) noexcept {
    struct stat st{};const int flags=fcntl(fd,F_GETFL),seals=fcntl(fd,F_GET_SEALS);
    if(fd<0||flags<0||(flags&O_ACCMODE)!=O_RDONLY||seals<0||(seals&(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))!=(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)
        ||fstat(fd,&st)||st.st_uid!=geteuid()||st.st_size!=SharedBytes)return false;
    auto* mapped=mmap(nullptr,SharedBytes,PROT_READ,MAP_SHARED,fd,0);if(mapped==MAP_FAILED)return false;
    const auto* shared=static_cast<const Shared*>(mapped);
    const auto epoch=shared->active.load(std::memory_order_acquire);
    if(!epoch||shared->magic.load(std::memory_order_acquire)!=Magic||shared->active.load(std::memory_order_acquire)!=epoch){munmap(mapped,SharedBytes);return false;}
    reset();impl_->shared=shared;impl_->epoch=epoch;return true;
}
bool LeaseReader::current(const LeaseReference& ref) const noexcept {
    Words values{};std::uint64_t sequence=0;return ref.capture.kind==nd::CaptureKind::Decoded&&ref.epoch==impl_->epoch&&snapshot(impl_->shared,ref.slot,impl_->epoch,sequence,values)
        &&sequence==ref.sequence&&values==words(ref.capture,ref.kernel);
}
PeerPin vrlinkPin() noexcept{return {2425024,nd::ExpectedVrlinkHash};}
PeerPin compositorPin() noexcept{return {6026096,{{0x0b,0xb1,0x48,0xed,0xa6,0xb5,0xe5,0x4e,0x2f,0xbe,0xd0,0x9d,0x47,0x5a,0x43,0x67,0x1f,0x5f,0xf6,0xfc,0xb6,0xc2,0xb4,0xf4,0x83,0x45,0xb1,0xa9,0xfe,0x8d,0x6f,0x96}}};}
namespace {
bool verifyPeer(int socket,const PeerPin& pin){
    ucred cred{};socklen_t length=sizeof cred;if(!pin.bytes||pin.bytes>8*1024*1024||getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&cred,&length)
        ||length!=sizeof cred||cred.uid!=geteuid()||cred.pid<=0)return false;
    char path[64];std::snprintf(path,sizeof path,"/proc/%d/exe",cred.pid);const int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    struct stat before{},after{};bool okay=fstat(fd,&before)==0&&before.st_size>=0&&static_cast<std::size_t>(before.st_size)==pin.bytes;
    frame_receiver::Sha256 hash;std::array<std::uint8_t,16384> buffer{};std::size_t total=0;
    while(okay&&total<pin.bytes){const auto n=read(fd,buffer.data(),std::min(buffer.size(),pin.bytes-total));if(n<=0){okay=false;break;}hash.update(buffer.data(),static_cast<std::size_t>(n));total+=static_cast<std::size_t>(n);}
    okay=okay&&fstat(fd,&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size
        &&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&hash.finish()==pin.sha256;close(fd);return okay;
}
int privateParent(const std::filesystem::path& path){
    if(!path.is_absolute()||path.filename().empty())return -1;
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;
    for(const auto& component:path.parent_path().relative_path()){
        if(component==".."||component=="."){close(fd);return -1;}const auto name=component.string();
        const int next=openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(fd);fd=next;if(fd<0)return -1;
    }
    struct stat st{};if(fstat(fd,&st)==0&&st.st_uid==geteuid()&&(st.st_mode&0777)==0700)return fd;
    close(fd);return -1;
}
bool sameFile(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_mode==b.st_mode&&a.st_uid==b.st_uid;}
int endpointLock(int parent,const std::string& name){
    constexpr char marker[]="SFNP_LOCAL_CHANNEL_LOCK/1\n";
    bool created=true;int fd=openat(parent,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0&&errno==EEXIST){created=false;fd=openat(parent,name.c_str(),O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);}
    if(fd<0)return -1;
    struct stat st{};bool okay=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&(st.st_mode&0777)==0600&&st.st_nlink==1
        &&st.st_size==(created?0:static_cast<off_t>(sizeof marker-1))&&flock(fd,LOCK_EX|LOCK_NB)==0;
    if(okay&&created)okay=pwrite(fd,marker,sizeof marker-1,0)==static_cast<ssize_t>(sizeof marker-1);
    if(okay){std::array<char,sizeof marker-1> actual{};okay=pread(fd,actual.data(),actual.size(),0)==static_cast<ssize_t>(actual.size())&&!std::memcmp(actual.data(),marker,actual.size());}
    if(okay){struct stat current{};okay=!fstatat(parent,name.c_str(),&current,AT_SYMLINK_NOFOLLOW)&&sameFile(st,current);}
    if(okay)return fd;
    close(fd);return -1;
}
bool currentEndpointLock(const std::filesystem::path& path,int parent,int lock,const std::string& lockName){
    const int current=privateParent(path);if(current<0)return false;
    struct stat a{},b{},held{},named{};const bool okay=!fstat(parent,&a)&&!fstat(current,&b)&&sameFile(a,b)
        &&!fstat(lock,&held)&&!fstatat(parent,lockName.c_str(),&named,AT_SYMLINK_NOFOLLOW)&&sameFile(held,named)&&held.st_nlink==1;
    close(current);return okay;
}
bool absentKernelEndpoint(const std::string& path){
    // Avoid even connecting to a live old-version listener which has no lock
    // yet. A connection probe could make its strict peer policy reject it.
    auto* file=std::fopen("/proc/net/unix","r");if(!file)return false;
    char line[512];unsigned lines=0;bool absent=true;
    while(std::fgets(line,sizeof line,file)){
        if(++lines>16384||!std::strchr(line,'\n')){absent=false;break;}
        int at=0;std::sscanf(line,"%*s %*s %*s %*s %*s %*s %*s %n",&at);
        if(at>0){auto* end=std::strchr(line+at,'\n');if(end)*end=0;if(path==line+at){absent=false;break;}}
    }
    if(std::ferror(file))absent=false;
    std::fclose(file);return absent;
}
bool reclaimOrphan(const std::filesystem::path& path,int parent,int lock,const std::string& lockName,const sockaddr_un& address){
    struct stat before{};const auto name=path.filename().string();
    if(fstatat(parent,name.c_str(),&before,AT_SYMLINK_NOFOLLOW))return errno==ENOENT;
    if(!S_ISSOCK(before.st_mode)||before.st_uid!=geteuid()||(before.st_mode&0777)!=0600||before.st_nlink!=1
        ||!absentKernelEndpoint(path.string()))return false;
    const int probe=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(probe<0)return false;
    const int result=connect(probe,reinterpret_cast<const sockaddr*>(&address),sizeof address);const int error=errno;close(probe);
    if(result==0||error!=ECONNREFUSED)return false;
    struct stat after{};return currentEndpointLock(path,parent,lock,lockName)
        &&!fstatat(parent,name.c_str(),&after,AT_SYMLINK_NOFOLLOW)&&sameFile(before,after)
        &&unlinkat(parent,name.c_str(),0)==0;
}
bool validMessage(const Message& m){
    if(!nonzero(m.source.stream.connection)||!m.source.outputPoolGeneration||!m.source.stream.generation
        ||!nonzero(m.binding)||!m.descriptor.sourceFrame||!m.descriptor.encoderGeneration||!m.descriptor.codedWidth||!m.descriptor.codedHeight
        ||m.descriptor.codedWidth>8192||m.descriptor.codedHeight>8192||m.nativeTimestampToken>0xffff||!m.submittedContent)return false;
    if(m.kind==Kind::Certified)return !m.certifiedMetadata.empty()&&m.certifiedMetadata.size()<=Channel::MaximumPayload&&m.dmabuf<0&&m.readOnlyLeases<0;
    if(m.kind!=Kind::Decoded||!m.certifiedMetadata.empty()||m.dmabuf<0||m.readOnlyLeases<0||m.lease.slot>=SlotCount||!m.lease.epoch||!m.lease.sequence||(m.lease.sequence&1))return false;
    const auto& c=m.lease.capture;return c.kind==nd::CaptureKind::Decoded&&c.sourcePoolGeneration==m.source.outputPoolGeneration
        &&c.nativeTimestampToken==m.nativeTimestampToken&&c.fd>=0&&c.dmabufFd>=0&&c.index<nd::MaxBuffers
        &&c.capturePoolGeneration&&c.contentGeneration&&m.lease.kernel[1];
}
std::array<std::uint8_t,HeaderBytes> encode(const Message& m){
    std::array<std::uint8_t,HeaderBytes> h{};std::memcpy(h.data(),"SFL1",4);h[4]=1;h[5]=static_cast<std::uint8_t>(m.kind);h[6]=m.peerConfirmed?1:0;
    put(h.data()+8,m.certifiedMetadata.size(),4);std::copy(m.source.stream.connection.begin(),m.source.stream.connection.end(),h.begin()+16);
    put(h.data()+32,m.source.outputPoolGeneration,8);put(h.data()+40,m.descriptor.sourceFrame,8);put(h.data()+48,m.descriptor.encoderGeneration,8);
    put(h.data()+56,m.descriptor.codedWidth,2);put(h.data()+58,m.descriptor.codedHeight,2);put(h.data()+60,m.nativeTimestampToken,4);put(h.data()+64,m.submittedContent,8);
    std::copy(m.binding.begin(),m.binding.end(),h.begin()+72);
    put(h.data()+144,m.source.stream.generation,8);
    if(m.kind==Kind::Decoded){put(h.data()+152,m.lease.epoch,8);put(h.data()+88,m.lease.slot,4);put(h.data()+92,m.lease.capture.index,4);put(h.data()+96,m.lease.sequence,8);
        put(h.data()+104,m.lease.capture.capturePoolGeneration,8);put(h.data()+112,m.lease.capture.contentGeneration,8);
        put(h.data()+120,m.lease.kernel[0],8);put(h.data()+128,m.lease.kernel[1],8);
        put(h.data()+136,static_cast<std::uint64_t>(m.lease.capture.fd),4);put(h.data()+140,static_cast<std::uint64_t>(m.lease.capture.dmabufFd),4);}
    return h;
}
bool decode(const std::array<std::uint8_t,HeaderBytes>& h,Message& m){
    if(std::memcmp(h.data(),"SFL1",4)||h[4]!=1||(h[5]!=1&&h[5]!=2)||h[6]>1||h[7])return false;
    for(unsigned i=12;i<16;++i)if(h[i])return false;
    for(unsigned i=160;i<HeaderBytes;++i)if(h[i])return false;
    const auto payload=get(h.data()+8,4);if(payload>Channel::MaximumPayload)return false;
    m.kind=static_cast<Kind>(h[5]);m.peerConfirmed=h[6]!=0;std::copy_n(h.begin()+16,16,m.source.stream.connection.begin());
    m.source.outputPoolGeneration=get(h.data()+32,8);m.source.stream.generation=get(h.data()+144,8);m.descriptor={get(h.data()+40,8),get(h.data()+48,8),static_cast<std::uint16_t>(get(h.data()+56,2)),static_cast<std::uint16_t>(get(h.data()+58,2))};
    m.nativeTimestampToken=static_cast<std::uint32_t>(get(h.data()+60,4));m.submittedContent=get(h.data()+64,8);std::copy_n(h.begin()+72,16,m.binding.begin());
    if(m.kind==Kind::Decoded){m.lease.epoch=get(h.data()+152,8);m.lease.slot=static_cast<std::uint32_t>(get(h.data()+88,4));m.lease.sequence=get(h.data()+96,8);m.lease.kernel={{get(h.data()+120,8),get(h.data()+128,8)}};
        m.lease.capture={nd::CaptureKind::Decoded,static_cast<int>(get(h.data()+136,4)),static_cast<int>(get(h.data()+140,4)),static_cast<std::uint32_t>(get(h.data()+92,4)),m.nativeTimestampToken,m.source.outputPoolGeneration,get(h.data()+104,8),get(h.data()+112,8)};
        if(payload)return false;
    }else {for(unsigned i=88;i<144;++i)if(h[i])return false;for(unsigned i=152;i<160;++i)if(h[i])return false;if(!payload)return false;}
    m.certifiedMetadata.resize(payload);return true;
}
}
struct Channel::Impl {
    struct Queued {bool used=false;std::array<std::uint8_t,HeaderBytes> header{};std::vector<std::uint8_t> payload;std::size_t at=0;int dma=-1,lease=-1;};
    Endpoint endpoint=Endpoint::CompositorConsumer;State state=State::Off;PeerPin pin{};
    int listen=-1,socket=-1,parent=-1,ownerLock=-1;std::filesystem::path path;std::string lockName;KernelIdentity socketIdentity{};bool ownsPath=false;
    std::array<Queued,2> outgoing{};std::size_t head=0,count=0;
    std::array<std::uint8_t,HeaderBytes> header{};std::size_t headerAt=0,payloadAt=0;Message assembling{};bool decoded=false,ready=false;
    void close(State next){closeFd(socket);closeFd(listen);for(auto& q:outgoing){closeFd(q.dma);closeFd(q.lease);q={};}head=count=0;
        closeFd(assembling.dmabuf);closeFd(assembling.readOnlyLeases);assembling={};header={};headerAt=payloadAt=0;decoded=ready=false;state=next;}
};
Channel::Channel():impl_(new Impl){}Channel::~Channel(){stop();}
bool Channel::prepare(Endpoint endpoint,const std::filesystem::path& path) noexcept{return preparePinned(endpoint,path,endpoint==Endpoint::VrlinkProducer?compositorPin():vrlinkPin());}
bool Channel::preparePinned(Endpoint endpoint,const std::filesystem::path& path,const PeerPin& pin) noexcept {
    stop();try{
        impl_->parent=privateParent(path);
        if(impl_->parent<0||path.string().size()>=sizeof(sockaddr_un::sun_path)||!pin.bytes||pin.bytes>8*1024*1024){stop();return false;}
        sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,path.c_str());
        const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return false;
        impl_->endpoint=endpoint;impl_->pin=pin;impl_->path=path;
        if(endpoint==Endpoint::CompositorConsumer){
            impl_->lockName=path.filename().string()+".lock";impl_->ownerLock=endpointLock(impl_->parent,impl_->lockName);
            if(impl_->ownerLock<0||!reclaimOrphan(path,impl_->parent,impl_->ownerLock,impl_->lockName,address)){close(fd);stop();impl_->state=State::Refused;return false;}
            if(bind(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address)){close(fd);impl_->state=State::Refused;return false;}
            struct stat st{};if(lstat(path.c_str(),&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()){close(fd);impl_->state=State::Refused;return false;}
            impl_->listen=fd;impl_->ownsPath=true;impl_->socketIdentity={{static_cast<std::uint64_t>(st.st_dev),static_cast<std::uint64_t>(st.st_ino)}};
            if(chmod(path.c_str(),0600)||listen(fd,1)){stop();impl_->state=State::Refused;return false;}
            impl_->state=State::Listening;return true;
        }
        struct stat st{};if(lstat(path.c_str(),&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0777)!=0600){close(fd);impl_->state=State::Refused;return false;}
        impl_->socket=fd;const int result=connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address);
        if(result!=0&&!pending()){impl_->close(State::Refused);return false;}
        impl_->state=State::Connecting;return true;
    }catch(...){impl_->close(State::Refused);return false;}
}
void Channel::stop() noexcept {
    impl_->close(State::Off);
    if(impl_->ownsPath){struct stat st{};if(lstat(impl_->path.c_str(),&st)==0&&S_ISSOCK(st.st_mode)&&st.st_uid==geteuid()
        &&static_cast<std::uint64_t>(st.st_dev)==impl_->socketIdentity[0]&&static_cast<std::uint64_t>(st.st_ino)==impl_->socketIdentity[1])unlink(impl_->path.c_str());}
    impl_->ownsPath=false;impl_->path.clear();impl_->socketIdentity={};
    closeFd(impl_->ownerLock);closeFd(impl_->parent);impl_->lockName.clear();
}
State Channel::state()const noexcept{return impl_->state;}
bool Channel::enqueue(const Message& message) noexcept {
    if(impl_->endpoint!=Endpoint::VrlinkProducer||impl_->state!=State::Connected||impl_->count==impl_->outgoing.size()||!validMessage(message))return false;
    try{auto& q=impl_->outgoing[(impl_->head+impl_->count)%impl_->outgoing.size()];
        q.header=encode(message);q.payload=message.certifiedMetadata;
        if(message.kind==Kind::Decoded){q.dma=duplicate(message.dmabuf);q.lease=duplicate(message.readOnlyLeases);KernelIdentity copied{};
            if(q.dma<0||q.lease<0||!identity(q.dma,copied)||copied!=message.lease.kernel){closeFd(q.dma);closeFd(q.lease);q={};return false;}}
        q.used=true;++impl_->count;return true;
    }catch(...){return false;}
}
void Channel::tick() noexcept {
    try{
        if(impl_->state==State::Listening){const int fd=accept4(impl_->listen,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if(fd<0){if(!pending())impl_->close(State::Lost);return;}
            if(!verifyPeer(fd,impl_->pin)){close(fd);impl_->close(State::Refused);return;}
            impl_->socket=fd;closeFd(impl_->listen);impl_->state=State::Connected;}
        if(impl_->state==State::Connecting){int error=0;socklen_t size=sizeof error;
            if(getsockopt(impl_->socket,SOL_SOCKET,SO_ERROR,&error,&size)||error){impl_->close(State::Lost);return;}
            if(!verifyPeer(impl_->socket,impl_->pin)){impl_->close(State::Refused);return;}impl_->state=State::Connected;}
        if(impl_->state!=State::Connected)return;
        if(impl_->endpoint==Endpoint::VrlinkProducer){
            // This channel is intentionally one-way. Check peer teardown even
            // with no queued frame, without consuming bytes or ever waiting.
            std::uint8_t unexpected=0;const auto probed=recv(impl_->socket,&unexpected,1,MSG_PEEK|MSG_DONTWAIT);
            if(probed==0){impl_->close(State::Lost);return;}
            if(probed>0){impl_->close(State::Refused);return;}
            if(errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK){impl_->close(State::Lost);return;}
            for(unsigned attempt=0;attempt<8&&impl_->count;++attempt){auto& q=impl_->outgoing[impl_->head];
                const bool header=q.at<HeaderBytes;const auto at=header?q.at:q.at-HeaderBytes;
                const auto* data=header?q.header.data():q.payload.data();const auto size=header?HeaderBytes:q.payload.size();
                iovec io{const_cast<std::uint8_t*>(data+at),std::min<std::size_t>(65536,size-at)};msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;
                alignas(cmsghdr) std::array<char,CMSG_SPACE(sizeof(int)*2)> control{};
                if(q.at==0&&q.dma>=0){message.msg_control=control.data();message.msg_controllen=control.size();auto* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int)*2);int fds[]{q.dma,q.lease};std::memcpy(CMSG_DATA(c),fds,sizeof fds);}
                const auto n=sendmsg(impl_->socket,&message,MSG_NOSIGNAL);if(n<0){if(pending())break;impl_->close(State::Lost);return;}if(!n){impl_->close(State::Lost);return;}q.at+=static_cast<std::size_t>(n);
                if(q.at==HeaderBytes+q.payload.size()){closeFd(q.dma);closeFd(q.lease);q={};impl_->head=(impl_->head+1)%impl_->outgoing.size();--impl_->count;}
            }return;
        }
        for(unsigned attempt=0;attempt<8&&!impl_->ready;++attempt){
            const bool header=impl_->headerAt<HeaderBytes;auto* data=header?impl_->header.data():impl_->assembling.certifiedMetadata.data();
            const auto at=header?impl_->headerAt:impl_->payloadAt,size=header?HeaderBytes:impl_->assembling.certifiedMetadata.size();
            iovec io{data+at,std::min<std::size_t>(65536,size-at)};msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;alignas(cmsghdr)std::array<char,CMSG_SPACE(sizeof(int)*8)> control{};message.msg_control=control.data();message.msg_controllen=control.size();
            const auto n=recvmsg(impl_->socket,&message,MSG_CMSG_CLOEXEC);if(n<0){if(pending())break;impl_->close(State::Lost);return;}if(!n){impl_->close(State::Lost);return;}
            bool bad=(message.msg_flags&(MSG_CTRUNC|MSG_TRUNC))!=0;
            for(auto* c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)){
                if(c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS||c->cmsg_len<CMSG_LEN(0)){bad=true;continue;}
                const auto bytes=c->cmsg_len-CMSG_LEN(0),count=bytes/sizeof(int);const auto* fds=reinterpret_cast<const int*>(CMSG_DATA(c));
                if(!header||impl_->headerAt!=0||bytes%sizeof(int)||count!=2||impl_->assembling.dmabuf>=0){for(std::size_t i=0;i<count;++i)::close(fds[i]);bad=true;continue;}
                impl_->assembling.dmabuf=fds[0];impl_->assembling.readOnlyLeases=fds[1];
            }
            if(bad){impl_->close(State::Refused);return;}
            if(header){impl_->headerAt+=static_cast<std::size_t>(n);
                if(impl_->headerAt==HeaderBytes){if(!decode(impl_->header,impl_->assembling)){impl_->close(State::Refused);return;}impl_->decoded=true;
                    KernelIdentity imported{};
                    if(!validMessage(impl_->assembling)||(impl_->assembling.kind==Kind::Decoded
                        &&(!identity(impl_->assembling.dmabuf,imported)||imported!=impl_->assembling.lease.kernel))){impl_->close(State::Refused);return;}
                    if(impl_->assembling.certifiedMetadata.empty())impl_->ready=true;}
            }else {impl_->payloadAt+=static_cast<std::size_t>(n);if(impl_->payloadAt==size)impl_->ready=true;}
        }
    }catch(...){impl_->close(State::Refused);}
}
bool Channel::receive(Message& output) noexcept {
    if(impl_->endpoint!=Endpoint::CompositorConsumer||!impl_->ready)return false;
    output=std::move(impl_->assembling);impl_->assembling={};impl_->header={};impl_->headerAt=impl_->payloadAt=0;impl_->decoded=impl_->ready=false;return true;
}
}
