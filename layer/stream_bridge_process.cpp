#include "stream_bridge_process.h"
#include "stream_bridge_artifacts.h"
#include "pixel_snapshot_io.h"
#include "../receiver/stream_bridge_native_loader.h"
#include "../bridge/stream_bridge_credentials.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <unistd.h>
namespace sb=stream_bridge;
namespace local=stream_bridge_local;
namespace native=stream_bridge_native;
namespace stream_bridge_renderer {
namespace {
struct Process {
    std::mutex gate;std::atomic<ProcessKind> kind{ProcessKind::Off};int directory=-1;
    std::filesystem::path root;std::array<ProcessConsumer,4> consumers{};
    local::Channel channel;local::LeaseWriter writer;native::NativeImages images;
    sb::AuthSession verifier;sb::VideoStreamContext verifiedSource{};
    std::array<local::Message,16> masks{};std::array<LocalPicture,16> pictures{};
    std::shared_ptr<local::LeaseReader> currentLeases;
    std::shared_ptr<std::atomic<bool>> connection;std::uint64_t lastConnect=0;
    std::thread worker;std::uint32_t notes=0;bool prepared=false;
    std::array<std::uint32_t,9> sourceNotices{}; // Written only by the receiver worker.
};
Process* process() noexcept{
    // The optional pre-main companion must not terminate Steam on allocation
    // failure, and failure is sticky without retrying from a noexcept callback.
    static auto* value=[]() noexcept -> Process*{try{return new Process;}catch(...){return nullptr;}}();
    return value;
}
bool sameDescriptor(const sb::FrameDescriptor&a,const sb::FrameDescriptor&b){return a.sourceFrame==b.sourceFrame&&a.encoderGeneration==b.encoderGeneration&&a.codedWidth==b.codedWidth&&a.codedHeight==b.codedHeight;}
bool sameSource(const sb::VideoStreamContext&a,const sb::VideoStreamContext&b){return a.connection==b.connection&&a.generation==b.generation;}
bool selfMatches(const local::PeerPin& pin){
    const int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    struct stat before{},after{};bool good=!fstat(fd,&before)&&S_ISREG(before.st_mode)&&before.st_size==static_cast<off_t>(pin.bytes);
    frame_receiver::Sha256 hash;std::array<unsigned char,65536> bytes{};std::size_t total=0;
    while(good&&total<pin.bytes){const auto n=read(fd,bytes.data(),std::min(bytes.size(),pin.bytes-total));if(n<0&&errno==EINTR)continue;if(n<=0){good=false;break;}hash.update(bytes.data(),std::size_t(n));total+=std::size_t(n);}
    good=good&&!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec;close(fd);
    return good&&total==pin.bytes&&hash.finish()==pin.sha256;
}
bool ownedRead(int directory,const char* name,void* output,std::size_t size){
    const int fd=openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return false;
    struct stat before{},after{};bool good=!fstat(fd,&before)&&S_ISREG(before.st_mode)&&before.st_uid==getuid()&&(before.st_mode&0777)==0600&&before.st_nlink==1&&before.st_size==static_cast<off_t>(size);
    std::size_t n=0;while(good&&n<size){const auto got=read(fd,static_cast<char*>(output)+n,size-n);if(got<0&&errno==EINTR)continue;if(got<=0){good=false;break;}n+=std::size_t(got);}
    good=good&&!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec;close(fd);return good&&n==size;
}
void note(Process&p,const char* event){if(p.notes++<32)std::fprintf(stderr,"[Steam Link Native Passthrough] %s\n",event);}
void certified(const native::CertifiedFrame& frame,void* context) noexcept{
    auto&p=*static_cast<Process*>(context);if(!frame.certificate||!frame.peerConfirmed||!frame.certificate->originalAccessUnitVerified)return;
    try{local::Message message;message.kind=local::Kind::Certified;message.source=frame.source;message.descriptor=frame.certificate->descriptor;message.binding=frame.certificate->binding;message.nativeTimestampToken=frame.submitted.nativeTimestampToken;message.submittedContent=frame.submitted.contentGeneration;message.peerConfirmed=true;
        if(!sb::serializeCoverageMetadata(*frame.certificate,message.certifiedMetadata))return;
        std::lock_guard<std::mutex> lock(p.gate);if(p.channel.enqueue(message))p.masks[message.descriptor.sourceFrame%p.masks.size()]=message;
    }catch(...){/* Missing local copy is opaque, never a guessed picture. */}
}
void decoded(const native::DecodedFrame& frame,void* context) noexcept{
    auto&p=*static_cast<Process*>(context);if(!frame.peerConfirmed)return;
    native::ImageLease image;local::LeaseReference lease;
    if(!p.images.join(frame.capture,image)||!p.writer.reference(frame.capture,lease)||!native::pinnedCaptureLeaseCurrent(frame.capture))return;
    try{std::lock_guard<std::mutex> lock(p.gate);
        // The exact certificate was queued first; recover its binding only by
        // matching this same source+submission+descriptor, never newest frame.
        local::Message message;bool found=false;
        for(const auto& old:p.masks)if(old.peerConfirmed&&sameSource(old.source.stream,frame.source.stream)&&sameDescriptor(old.descriptor,frame.descriptor)&&old.nativeTimestampToken==frame.submitted.nativeTimestampToken&&old.submittedContent==frame.submitted.contentGeneration){message.binding=old.binding;found=true;break;}
        if(!found)return;
        message.kind=local::Kind::Decoded;message.source=frame.source;message.descriptor=frame.descriptor;message.nativeTimestampToken=frame.submitted.nativeTimestampToken;message.submittedContent=frame.submitted.contentGeneration;message.peerConfirmed=true;message.lease=lease;message.dmabuf=frame.capture.dmabufFd;message.readOnlyLeases=p.writer.duplicateReadOnly();
        if(message.readOnlyLeases>=0){p.channel.enqueue(message);close(message.readOnlyLeases);}
    }catch(...){/* Missing selected lease is ordinary native output. */}
}
void sourceNotice(const native::RuntimeNotice& notice,void* context) noexcept{
    auto&p=*static_cast<Process*>(context);if(notice.state==native::RuntimeState::SourceLost||notice.state==native::RuntimeState::SenderUnavailable)p.writer.invalidateCurrentEpoch();
    // Worker-only, first occurrence of each state/control pair. Repeated
    // reconnects cannot flood the journal or hide a later admission failure.
    const auto state=static_cast<unsigned>(notice.state),control=static_cast<unsigned>(notice.control);
    if(state>=p.sourceNotices.size()||control>=32)return;
    const auto bit=std::uint32_t(1)<<control;if(p.sourceNotices[state]&bit)return;
    p.sourceNotices[state]|=bit;
    constexpr const char* names[]{"disabled","waiting-native-source","waiting-local-image-route","negotiating",
        "peer-confirmed","source-proof-lost","sender-unavailable","pairing-refused","source-refused"};
    std::fprintf(stderr,"[Steam Link Native Passthrough] receiver %s; control=%s\n",names[state],sb::controlStatusName(notice.control));
}
void consume(Process&p,local::Message& message){
    if(!message.peerConfirmed)return;
    if(message.kind==local::Kind::Certified){
        if(!sameSource(p.verifiedSource,message.source.stream)){
            sb::Pairing pair;const auto status=sb::loadPrivatePairing(p.root/"pairings",sb::Role::Receiver,pair);
            if(status!=sb::CredentialStatus::Ok)return;
            const auto ready=p.verifier.prepareReceiver(pair,message.source.stream);sb::secureZero(pair.key.data(),pair.key.size());if(ready!=sb::AuthStatus::Ok)return;
            p.verifiedSource=message.source.stream;for(auto& old:p.masks)old={};for(auto& old:p.pictures)old={};p.currentLeases.reset();
        }
        sb::CertifiedAccessUnit certificate;if(sb::inspectCoverageMetadata({message.certifiedMetadata.data(),message.certifiedMetadata.size()},p.verifier,certificate)!=sb::FrameStatus::Ok||certificate.binding!=message.binding||!sameDescriptor(certificate.descriptor,message.descriptor))return;
        auto* target=&p.masks[message.descriptor.sourceFrame%p.masks.size()];*target=message;
        for(const auto& consumer:p.consumers)if(consumer.owner&&(!consumer.admitted||consumer.admitted(consumer.owner))&&consumer.coverage)consumer.coverage(consumer.owner,certificate);
    }else if(message.kind==local::Kind::Decoded){
        bool certified=false;for(const auto& mask:p.masks)if(mask.peerConfirmed&&mask.binding==message.binding&&sameSource(mask.source.stream,message.source.stream)&&sameDescriptor(mask.descriptor,message.descriptor)&&mask.nativeTimestampToken==message.nativeTimestampToken&&mask.submittedContent==message.submittedContent){certified=true;break;}
        if(!certified||message.dmabuf<0||message.readOnlyLeases<0)return;
        auto reader=std::make_shared<local::LeaseReader>();if(!reader->mapReadOnly(message.readOnlyLeases)||!reader->current(message.lease))return;
        struct stat file{};if(fstat(message.dmabuf,&file)||local::KernelIdentity{{std::uint64_t(file.st_dev),std::uint64_t(file.st_ino)}}!=message.lease.kernel)return;
        LocalPicture picture;picture.metadata=message;picture.metadata.dmabuf=picture.metadata.readOnlyLeases=-1;picture.leases=reader;picture.connection=p.connection;
        auto& old=p.pictures[message.descriptor.sourceFrame%p.pictures.size()];old=std::move(picture);
        for(const auto& consumer:p.consumers)if(consumer.owner&&(!consumer.admitted||consumer.admitted(consumer.owner))&&consumer.picture)consumer.picture(consumer.owner,old);
    }
}
}
namespace {
bool imageRoutePrepared(void* context) noexcept{
    auto&p=*static_cast<Process*>(context);std::lock_guard<std::mutex> lock(p.gate);return p.channel.state()==local::State::Connected;
}
void workCycle(Process& p,local::State& previous) noexcept{
    try{
        std::lock_guard<std::mutex> lock(p.gate);
        const auto state=p.channel.state();
        const bool start=p.kind==ProcessKind::Vrlink||std::any_of(p.consumers.begin(),p.consumers.end(),[](const auto& c){return c.owner&&(!c.admitted||c.admitted(c.owner));});
        if(!start&&state!=local::State::Off){
            if(p.connection)p.connection->store(false,std::memory_order_release);
            p.channel.stop();
        }
        const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if(start&&(state==local::State::Off||state==local::State::Lost||state==local::State::Refused)&&now-p.lastConnect>=1000){
            p.lastConnect=now;p.channel.stop();p.channel.prepare(p.kind==ProcessKind::Vrlink?local::Endpoint::VrlinkProducer:local::Endpoint::CompositorConsumer,p.root/"receiver.sock");
        }
        p.channel.tick();const auto current=p.channel.state();
        if(current!=previous){
            if(p.connection)p.connection->store(false,std::memory_order_release);
            if(current==local::State::Connected)p.connection=std::make_shared<std::atomic<bool>>(true);
            if(current==local::State::Lost||current==local::State::Refused)p.writer.invalidateCurrentEpoch();
            previous=current;note(p,current==local::State::Connected?"local image bridge connected":"local image bridge waiting; ordinary video retained");
        }
        if(p.kind==ProcessKind::Compositor){
            local::Message message;
            for(unsigned n=0;n<2&&p.channel.receive(message);++n){
                // Consumer allocation failure must still close this packet's
                // received SCM_RIGHTS. Never leak decoder objects on OOM.
                try{consume(p,message);}catch(...){}
                if(message.dmabuf>=0)close(message.dmabuf);
                if(message.readOnlyLeases>=0)close(message.readOnlyLeases);
                message={};
            }
            for(const auto& consumer:p.consumers)if(consumer.owner&&(!consumer.admitted||consumer.admitted(consumer.owner))){
                for(const auto& picture:p.pictures)if(picture.leases&&picture.connection&&picture.connection->load(std::memory_order_acquire)&&picture.leases->current(picture.metadata.lease)&&consumer.picture)consumer.picture(consumer.owner,picture);
                if(consumer.tick)consumer.tick(consumer.owner);
            }
        }
    }catch(...){/* Optional worker loss cannot stop native rendering. */}
}
void work(Process* pointer) noexcept{
    local::State previous=local::State::Off;
    for(;;){workCycle(*pointer,previous);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
bool fragment(const char* name,std::size_t bytes,const char* sha,std::vector<std::uint32_t>& output){
    auto* pointer=process();if(!pointer)return false;auto&p=*pointer;if(p.directory<0||bytes%4)return false;
    std::vector<std::uint32_t> words(bytes/4);if(!ownedRead(p.directory,name,words.data(),bytes))return false;
    if(frame_pixel_io::digest(words.data(),bytes)!=sha)return false;
    output.swap(words);return true;
}
// The tiny launcher injects the heavy library only into the exact vrlink exec.
// Clean its own token before the first owned thread, preserving all foreign
// loader tokens; native internal child launches must not inherit this payload.
void cleanPayloadEnvironment(){
    Dl_info info{};if(!dladdr(reinterpret_cast<void*>(&prepareProcessCold),&info)||!info.dli_fname)return;
    const char* previous=std::getenv("LD_PRELOAD");if(!previous)return;
    std::string value(previous);const std::string self(info.dli_fname);
    for(std::size_t at=0;at<value.size();){
        if(value[at]==':'||value[at]==' '||value[at]=='\t'){++at;continue;}
        const auto end=value.find_first_of(": \t",at);const auto length=(end==std::string::npos?value.size():end)-at;
        if(value.compare(at,length,self)==0)value.erase(at,length);
        else at+=length;
    }
    if(value.find_first_not_of(": \t")==std::string::npos)unsetenv("LD_PRELOAD");else setenv("LD_PRELOAD",value.c_str(),1);
}
}
bool prepareProcessCold() noexcept{
    auto* pointer=process();if(!pointer)return false;auto&p=*pointer;static std::once_flag once;
    try{std::call_once(once,[&]{
        ProcessKind kind=ProcessKind::Off;
        if(selfMatches(local::vrlinkPin()))kind=ProcessKind::Vrlink;
        else if(selfMatches(local::compositorPin()))kind=ProcessKind::Compositor;
        else return;
        cleanPayloadEnvironment();
        const auto* xdg=std::getenv("XDG_DATA_HOME");const auto* home=std::getenv("HOME");
        if(xdg&&xdg[0]=='/'&&strnlen(xdg,1025)<900)p.root=std::filesystem::path(xdg)/"SteamLinkNativePassthrough"/"stream-bridge";
        else if(home&&home[0]=='/'&&strnlen(home,1025)<850)p.root=std::filesystem::path(home)/".local/share/SteamLinkNativePassthrough/stream-bridge";
        else return;
        p.directory=frame_pixel_io::privateDirectory(p.root.string());if(p.directory<0)return;
        constexpr char magic[]="SFNP_STREAM_RECEIVER/1\n";std::array<char,sizeof(magic)-1> marker{};
        if(!ownedRead(p.directory,"receiver.enable",marker.data(),marker.size())||std::memcmp(marker.data(),magic,marker.size()))return;
        char config[128]{};struct stat st{};if(fstatat(p.directory,"receiver.conf",&st,AT_SYMLINK_NOFOLLOW)||st.st_size<static_cast<off_t>(sizeof(magic))||st.st_size>126||!ownedRead(p.directory,"receiver.conf",config,std::size_t(st.st_size)))return;
        if(std::memcmp(config,magic,sizeof(magic)-1))return;
        char address[16]{};unsigned port=0;int used=0;
        if(std::sscanf(config+sizeof(magic)-1,"%15[0-9.]\n%u\n%n",address,&port,&used)!=2||used!=st.st_size-static_cast<off_t>(sizeof(magic)-1)||!port||port>65535)return;
        p.kind.store(kind,std::memory_order_release);
        if(kind==ProcessKind::Vrlink){
            if(!p.writer.prepareCold()){p.kind=ProcessKind::Off;return;}
            native::RuntimeRequest request;request.explicitlyEnabled=true;request.buildId=712;request.pairingsDirectory=p.root/"pairings";std::copy_n(address,std::strlen(address)+1,request.producerAddress.data());request.producerPort=std::uint16_t(port);
            request.callbacks={certified,decoded,nullptr,sourceNotice,&p};request.imageRoutePrepared=imageRoutePrepared;request.imageRouteContext=&p;
            if(!native::startPinnedReceiver(request)||!native::setPinnedCaptureSink(local::LeaseWriter::sink,&p.writer)){native::stopPinnedReceiver();p.kind=ProcessKind::Off;return;}
        }
        p.prepared=true;p.worker=std::thread(work,&p);p.worker.detach();note(p,kind==ProcessKind::Vrlink?"early video source prepared":"native compositor bridge prepared");
    });}catch(...){p.prepared=false;p.kind=ProcessKind::Off;native::stopPinnedReceiver();}
    return p.prepared;
}
bool enabled() noexcept{const auto* p=process();return p&&p->kind.load(std::memory_order_acquire)!=ProcessKind::Off;}
ProcessKind processKind() noexcept{const auto* p=process();return p?p->kind.load(std::memory_order_acquire):ProcessKind::Off;}
native::NativeImages* sourceImages() noexcept{auto* p=process();return p&&p->kind==ProcessKind::Vrlink?&p->images:nullptr;}
void sourceExportedFd(int fd) noexcept{auto* p=process();if(p&&p->kind==ProcessKind::Vrlink)p->writer.associateResourceFd(fd);}
bool registerConsumerCold(ProcessConsumer consumer) noexcept{
    if(!consumer.owner||processKind()!=ProcessKind::Compositor)return false;
    auto* pointer=process();if(!pointer)return false;auto&p=*pointer;std::lock_guard<std::mutex> lock(p.gate);
    for(auto& slot:p.consumers)if(!slot.owner){slot=consumer;return true;}
    return false;
}
void unregisterConsumerCold(void* owner) noexcept{
    auto* pointer=process();if(!pointer)return;auto&p=*pointer;std::lock_guard<std::mutex> lock(p.gate);
    for(auto& slot:p.consumers)if(slot.owner==owner)slot={};
    if(std::none_of(p.consumers.begin(),p.consumers.end(),[](const auto& c){return c.owner&&(!c.admitted||c.admitted(c.owner));})){
        if(p.connection)p.connection->store(false,std::memory_order_release);
        p.channel.stop();
    }
}
bool readCoverageFragmentCold(std::vector<std::uint32_t>& output) noexcept{try{return fragment("coverage.spv",stream_bridge_artifacts::CoverageBytes,stream_bridge_artifacts::CoverageSha256,output);}catch(...){return false;}}
bool readLegacyFragmentCold(std::vector<std::uint32_t>& output) noexcept{try{return fragment("legacy.spv",stream_bridge_artifacts::LegacyBytes,stream_bridge_artifacts::LegacySha256,output);}catch(...){return false;}}
__attribute__((constructor)) static void prepareBeforeMain(){prepareProcessCold();}
} // namespace stream_bridge_renderer
