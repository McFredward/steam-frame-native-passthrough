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
#include <condition_variable>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
namespace sb=stream_bridge;
namespace local=stream_bridge_local;
namespace native=stream_bridge_native;
namespace stream_bridge_renderer {
namespace {
enum class Flow:unsigned {CertifiedCallback,CertifiedQueued,CertifiedQueueRefused,DecodedCallback,ImageMiss,LeaseMiss,NativeStale,BindingMiss,DecodedQueued,DecodedQueueRefused,CertifiedReceived,Authenticated,AuthenticationRefused,DecodedReceived,CertificateMiss,ReceivedLeaseMiss,KernelMiss,PicturesRetained,ReadyQueued,ReadyReceived,HandoffWait,HandoffReady,HandoffTimeout,Count};
struct Process {
    std::mutex gate;std::condition_variable released;std::atomic<bool> peerConfirmed{false};int wake=-1;std::atomic<ProcessKind> kind{ProcessKind::Off};int directory=-1;
    std::filesystem::path root;std::array<ProcessConsumer,4> consumers{};
    local::Channel channel;local::LeaseWriter writer;native::NativeImages images;
    sb::AuthSession verifier;sb::VideoStreamContext verifiedSource{};
    std::array<local::Message,native::ReceiverWorker::PendingCapacity> masks{};
    std::array<LocalPicture,native::MaxDevices*native::MaxBuffers> pictures{};
    std::array<local::Message,native::MaxDevices*native::MaxBuffers> awaiting{},acknowledged{};
    std::array<bool,native::ReceiverWorker::PendingCapacity> coverageNeeded{};
    std::array<bool,native::MaxDevices*native::MaxBuffers> readyQueued{};
    std::shared_ptr<local::LeaseReader> currentLeases;
    std::shared_ptr<std::atomic<bool>> connection;std::uint64_t lastConnect=0;
    std::thread worker;std::uint32_t notes=0;bool prepared=false;
    std::array<std::array<std::uint32_t,24>,9> sourceNotices{};unsigned sourceNoticeLines=0; // Written only by the receiver worker.
    std::uint32_t channelNotices=0; // Written only by the local channel worker.
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(Flow::Count)> flow{};
    std::uint64_t flowMilestones=0;unsigned flowLevel=0,flowLines=0; // Local worker only.
};
void count(Process& p,Flow flow){p.flow[static_cast<unsigned>(flow)].fetch_add(1,std::memory_order_relaxed);}
void reportFlowCold(Process& p){
    if(p.flowLines>=48)return;
    std::array<std::uint64_t,static_cast<unsigned>(Flow::Count)> values{};std::uint64_t milestones=0,progress=0;
    for(unsigned n=0;n<values.size();++n){values[n]=p.flow[n].load(std::memory_order_relaxed);if(values[n])milestones|=std::uint64_t(1)<<n;progress=std::max(progress,values[n]);}
    if(!milestones)return;
    unsigned level=0;for(auto value=progress;value>1;value>>=1)++level;
    if(!(milestones&~p.flowMilestones)&&level<=p.flowLevel)return;
    p.flowMilestones|=milestones;p.flowLevel=std::max(level,p.flowLevel);++p.flowLines;
    const auto get=[&](Flow f){return static_cast<unsigned long long>(values[static_cast<unsigned>(f)]);};
    std::fprintf(stderr,"[Steam Link Native Passthrough] local flow %s: certificate callbacks/queued/queue-refused=%llu/%llu/%llu; decoded callbacks/queued/queue-refused=%llu/%llu/%llu; source miss image/lease/current/binding=%llu/%llu/%llu/%llu; received certificate/authenticated/auth-refused=%llu/%llu/%llu; received decoded/retained=%llu/%llu; receive miss certificate/lease/kernel=%llu/%llu/%llu\n",
        p.kind==ProcessKind::Vrlink?"video":"compositor",get(Flow::CertifiedCallback),get(Flow::CertifiedQueued),get(Flow::CertifiedQueueRefused),get(Flow::DecodedCallback),get(Flow::DecodedQueued),get(Flow::DecodedQueueRefused),get(Flow::ImageMiss),get(Flow::LeaseMiss),get(Flow::NativeStale),get(Flow::BindingMiss),get(Flow::CertifiedReceived),get(Flow::Authenticated),get(Flow::AuthenticationRefused),get(Flow::DecodedReceived),get(Flow::PicturesRetained),get(Flow::CertificateMiss),get(Flow::ReceivedLeaseMiss),get(Flow::KernelMiss));
    std::fprintf(stderr,"[Steam Link Native Passthrough] paired decoder handoff %s: ready queued/received=%llu/%llu; waited/ready/emergency-timeout=%llu/%llu/%llu\n",
        p.kind==ProcessKind::Vrlink?"video":"compositor",get(Flow::ReadyQueued),get(Flow::ReadyReceived),get(Flow::HandoffWait),get(Flow::HandoffReady),get(Flow::HandoffTimeout));
}
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
void notify(Process& p) noexcept {if(p.wake>=0){const std::uint64_t one=1;const auto ignored=write(p.wake,&one,sizeof one);(void)ignored;}}
bool sameCapture(const native::CaptureObservation&a,const native::CaptureObservation&b) noexcept {
    return a.kind==b.kind&&a.fd==b.fd&&a.dmabufFd==b.dmabufFd&&a.index==b.index&&a.nativeTimestampToken==b.nativeTimestampToken
        &&a.sourcePoolGeneration==b.sourcePoolGeneration&&a.capturePoolGeneration==b.capturePoolGeneration&&a.contentGeneration==b.contentGeneration;
}
bool sameLease(const local::LeaseReference&a,const local::LeaseReference&b) noexcept {
    return a.slot==b.slot&&a.sequence==b.sequence&&a.epoch==b.epoch&&a.kernel==b.kernel&&sameCapture(a.capture,b.capture);
}
// The native decoder calls this only AFTER publishing its successful exact DQ
// and dropping its state lock, before exposing that image to the native player.
// Never wait on a compositor/render thread, a socket, or an unpaired stream.
void handoff(const native::CaptureObservation& capture,void* context) noexcept {
    auto&p=*static_cast<Process*>(context);if(!p.peerConfirmed.load(std::memory_order_acquire))return;
    try{
        native::wakePinnedReceiver();notify(p);
        std::unique_lock<std::mutex> lock(p.gate);
        const auto current=[&]{local::LeaseReference lease;return p.writer.reference(capture,lease);};
        const auto ready=[&]{local::LeaseReference lease;if(!p.writer.reference(capture,lease))return false;for(const auto& receipt:p.acknowledged)if(receipt.peerConfirmed&&sameLease(receipt.lease,lease))return true;return false;};
        if(ready()&&current()){count(p,Flow::HandoffReady);return;}
        if(!current()||!p.peerConfirmed.load(std::memory_order_acquire)||p.channel.state()!=local::State::Connected)return;
        count(p,Flow::HandoffWait);
        const bool woke=p.released.wait_for(lock,std::chrono::milliseconds(100),[&]{return ready()||!current()||!p.peerConfirmed.load(std::memory_order_acquire)||p.channel.state()!=local::State::Connected;});
        if(woke&&ready()&&current())count(p,Flow::HandoffReady);
        else if(!woke)count(p,Flow::HandoffTimeout);
    }catch(...){/* Genuine local failure preserves the original native result. */}
}
void certified(const native::CertifiedFrame& frame,void* context) noexcept{
    auto&p=*static_cast<Process*>(context);count(p,Flow::CertifiedCallback);if(!frame.certificate||!frame.peerConfirmed||!frame.certificate->originalAccessUnitVerified)return;
    try{local::Message message;message.kind=local::Kind::Certified;message.source=frame.source;message.descriptor=frame.certificate->descriptor;message.binding=frame.certificate->binding;message.nativeTimestampToken=frame.submitted.nativeTimestampToken;message.submittedContent=frame.submitted.contentGeneration;message.peerConfirmed=true;
        if(!sb::serializeCoverageMetadata(*frame.certificate,message.certifiedMetadata))return;
        std::lock_guard<std::mutex> lock(p.gate);if(p.channel.enqueue(message)){p.masks[message.descriptor.sourceFrame%p.masks.size()]=message;count(p,Flow::CertifiedQueued);notify(p);}else count(p,Flow::CertifiedQueueRefused);
    }catch(...){/* Missing local copy is opaque, never a guessed picture. */}
}
void decoded(const native::DecodedFrame& frame,void* context) noexcept{
    auto&p=*static_cast<Process*>(context);count(p,Flow::DecodedCallback);if(!frame.peerConfirmed)return;
    native::ImageLease image;local::LeaseReference lease;
    if(!p.images.join(frame.capture,image)){count(p,Flow::ImageMiss);return;}
    if(!p.writer.reference(frame.capture,lease)){count(p,Flow::LeaseMiss);return;}
    if(!native::pinnedCaptureLeaseCurrentOnWorker(frame.capture)){count(p,Flow::NativeStale);return;}
    try{std::lock_guard<std::mutex> lock(p.gate);
        // The exact certificate was queued first; recover its binding only by
        // matching this same source+submission+descriptor, never newest frame.
        local::Message message;bool found=false;
        for(const auto& old:p.masks)if(old.peerConfirmed&&sameSource(old.source.stream,frame.source.stream)&&sameDescriptor(old.descriptor,frame.descriptor)&&old.nativeTimestampToken==frame.submitted.nativeTimestampToken&&old.submittedContent==frame.submitted.contentGeneration){message.binding=old.binding;found=true;break;}
        if(!found){count(p,Flow::BindingMiss);return;}
        message.kind=local::Kind::Decoded;message.source=frame.source;message.descriptor=frame.descriptor;message.nativeTimestampToken=frame.submitted.nativeTimestampToken;message.submittedContent=frame.submitted.contentGeneration;message.peerConfirmed=true;message.lease=lease;message.dmabuf=frame.capture.dmabufFd;message.readOnlyLeases=p.writer.duplicateReadOnly();
        if(message.readOnlyLeases>=0){if(p.channel.enqueue(message)){
            count(p,Flow::DecodedQueued);auto receipt=message;receipt.dmabuf=receipt.readOnlyLeases=-1;
            if(lease.slot<p.awaiting.size()){p.awaiting[lease.slot]=std::move(receipt);p.acknowledged[lease.slot]={};}notify(p);
        }else count(p,Flow::DecodedQueueRefused);close(message.readOnlyLeases);}else count(p,Flow::LeaseMiss);
    }catch(...){/* Missing selected lease is ordinary native output. */}
}
void sourceNotice(const native::RuntimeNotice& notice,void* context) noexcept{
    auto&p=*static_cast<Process*>(context);
    {std::lock_guard<std::mutex> lock(p.gate);p.peerConfirmed.store(notice.state==native::RuntimeState::PeerConfirmed,std::memory_order_release);
        if(notice.state==native::RuntimeState::SourceLost||notice.state==native::RuntimeState::SenderUnavailable)p.writer.invalidateCurrentEpoch();}
    p.released.notify_all();
    // Worker-only, first occurrence of each state/control/reason, with an
    // overall process bound. Keep distinct native-loss causes visible without
    // journal growth proportional to frames or repeated connection attempts.
    const auto state=static_cast<unsigned>(notice.state),control=static_cast<unsigned>(notice.control),reason=static_cast<unsigned>(notice.reason);
    if(state>=p.sourceNotices.size()||control>=32||reason>=p.sourceNotices[state].size()||p.sourceNoticeLines>=64)return;
    const auto bit=std::uint32_t(1)<<control;if(p.sourceNotices[state][reason]&bit)return;
    p.sourceNotices[state][reason]|=bit;++p.sourceNoticeLines;
    constexpr const char* names[]{"disabled","waiting-native-source","waiting-local-image-route","negotiating",
        "peer-confirmed","source-proof-lost","sender-unavailable","pairing-refused","source-refused"};
        std::fprintf(stderr,"[Steam Link Native Passthrough] receiver %s; control=%s; reason=%s; pool=%llu; admission=%llu; snapshots=%llu/%llu/%llu; capture-lost=%llu\n",
        names[state],sb::controlStatusName(notice.control),native::runtimeReasonName(notice.reason),
        static_cast<unsigned long long>(notice.source.outputPoolGeneration),static_cast<unsigned long long>(notice.source.stream.generation),
        static_cast<unsigned long long>(notice.statistics.accepted),static_cast<unsigned long long>(notice.statistics.dropped),
        static_cast<unsigned long long>(notice.statistics.rejected),static_cast<unsigned long long>(notice.statistics.captureDropped));
}
void consume(Process&p,local::Message& message){
    if(!message.peerConfirmed)return;
    if(message.kind==local::Kind::Ready){
        if(p.kind!=ProcessKind::Vrlink||message.lease.slot>=p.awaiting.size())return;
        const auto& expected=p.awaiting[message.lease.slot];local::LeaseReference lease;
        if(!expected.peerConfirmed||expected.binding!=message.binding||!sameSource(expected.source.stream,message.source.stream)
            ||!sameDescriptor(expected.descriptor,message.descriptor)||expected.nativeTimestampToken!=message.nativeTimestampToken
            ||expected.submittedContent!=message.submittedContent||!sameLease(expected.lease,message.lease)
            ||!p.writer.reference(message.lease.capture,lease)||!sameLease(lease,message.lease))return;
        p.acknowledged[message.lease.slot]=message;count(p,Flow::ReadyReceived);p.released.notify_all();return;
    }
    if(message.kind==local::Kind::Certified){
        count(p,Flow::CertifiedReceived);
        if(!sameSource(p.verifiedSource,message.source.stream)){
            sb::Pairing pair;const auto status=sb::loadPrivatePairing(p.root/"pairings",sb::Role::Receiver,pair);
            if(status!=sb::CredentialStatus::Ok){count(p,Flow::AuthenticationRefused);return;}
            const auto ready=p.verifier.prepareReceiver(pair,message.source.stream);sb::secureZero(pair.key.data(),pair.key.size());if(ready!=sb::AuthStatus::Ok){count(p,Flow::AuthenticationRefused);return;}
            p.verifiedSource=message.source.stream;for(auto& old:p.masks)old={};for(auto& old:p.pictures)old={};p.currentLeases.reset();
        }
        sb::CertifiedAccessUnit certificate;if(sb::inspectCoverageMetadata({message.certifiedMetadata.data(),message.certifiedMetadata.size()},p.verifier,certificate)!=sb::FrameStatus::Ok||certificate.binding!=message.binding||!sameDescriptor(certificate.descriptor,message.descriptor)){count(p,Flow::AuthenticationRefused);return;}
        count(p,Flow::Authenticated);
        const auto index=message.descriptor.sourceFrame%p.masks.size();
        auto* target=&p.masks[index];*target=message;p.coverageNeeded[index]=certificate.coverageEncoding!=sb::CoverageEncoding::None;
        for(const auto& consumer:p.consumers)if(consumer.owner&&(!consumer.admitted||consumer.admitted(consumer.owner))&&consumer.coverage)consumer.coverage(consumer.owner,certificate);
    }else if(message.kind==local::Kind::Decoded){
        count(p,Flow::DecodedReceived);
        bool certified=false;for(const auto& mask:p.masks)if(mask.peerConfirmed&&mask.binding==message.binding&&sameSource(mask.source.stream,message.source.stream)&&sameDescriptor(mask.descriptor,message.descriptor)&&mask.nativeTimestampToken==message.nativeTimestampToken&&mask.submittedContent==message.submittedContent){certified=true;break;}
        if(!certified||message.dmabuf<0||message.readOnlyLeases<0){count(p,Flow::CertificateMiss);return;}
        auto reader=std::make_shared<local::LeaseReader>();if(!reader->mapReadOnly(message.readOnlyLeases)||!reader->current(message.lease)){count(p,Flow::ReceivedLeaseMiss);return;}
        struct stat file{};if(fstat(message.dmabuf,&file)||local::KernelIdentity{{std::uint64_t(file.st_dev),std::uint64_t(file.st_ino)}}!=message.lease.kernel){count(p,Flow::KernelMiss);return;}
        LocalPicture picture;picture.metadata=message;picture.metadata.dmabuf=picture.metadata.readOnlyLeases=-1;picture.leases=reader;picture.connection=p.connection;
        if(message.lease.slot>=p.pictures.size())return;
        auto& old=p.pictures[message.lease.slot];old=std::move(picture);p.readyQueued[message.lease.slot]=false;count(p,Flow::PicturesRetained);
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
            previous=current;p.released.notify_all();
            const auto bit=std::uint32_t(1)<<static_cast<unsigned>(current);
            if(!(p.channelNotices&bit)){
                p.channelNotices|=bit;
                switch(current){
                    case local::State::Connected:note(p,"local image bridge connected");break;
                    case local::State::Listening:note(p,"local image bridge listening; waiting for the video receiver");break;
                    case local::State::Connecting:note(p,"local image bridge connecting; ordinary video retained");break;
                    case local::State::Refused:note(p,"local image bridge refused; endpoint ownership or peer verification failed");break;
                    case local::State::Lost:note(p,"local image bridge lost; obsolete picture leases revoked");break;
                    case local::State::Capacity:note(p,"local image bridge capacity refused; ordinary video retained");break;
                    default:note(p,"local image bridge waiting; ordinary video retained");break;
                }
            }
        }
        // Drain a bounded complete burst in this cycle. A second receive
        // without tick cannot parse the next certificate/decoded packet.
        local::Message message;
        for(unsigned n=0;n<native::MaxBuffers*4&&p.channel.receive(message);++n){
            try{consume(p,message);}catch(...){}
            if(message.dmabuf>=0)close(message.dmabuf);
            if(message.readOnlyLeases>=0)close(message.readOnlyLeases);
            message={};p.channel.tick();
        }
        if(p.kind==ProcessKind::Compositor){
            for(const auto& consumer:p.consumers)if(consumer.owner&&(!consumer.admitted||consumer.admitted(consumer.owner))){
                if(consumer.tick)consumer.tick(consumer.owner);
                for(const auto& picture:p.pictures)if(picture.leases&&picture.connection&&picture.connection->load(std::memory_order_acquire)&&picture.leases->current(picture.metadata.lease)&&consumer.picture)consumer.picture(consumer.owner,picture);
            }
            for(std::size_t n=0;n<p.pictures.size();++n){const auto& picture=p.pictures[n];
                if(p.readyQueued[n]||!picture.leases||!picture.connection||!picture.connection->load(std::memory_order_acquire)||!picture.leases->current(picture.metadata.lease))continue;
                bool certificateFound=false,needsCoverage=true;
                for(std::size_t k=0;k<p.masks.size();++k)if(p.masks[k].peerConfirmed&&p.masks[k].binding==picture.metadata.binding
                    &&sameSource(p.masks[k].source.stream,picture.metadata.source.stream)
                    &&p.masks[k].source.outputPoolGeneration==picture.metadata.source.outputPoolGeneration
                    &&p.masks[k].nativeTimestampToken==picture.metadata.nativeTimestampToken&&p.masks[k].submittedContent==picture.metadata.submittedContent
                    &&sameDescriptor(p.masks[k].descriptor,picture.metadata.descriptor)){certificateFound=true;needsCoverage=p.coverageNeeded[k];break;}
                if(!certificateFound)continue;
                bool any=false,all=true;
                if(needsCoverage)for(const auto& consumer:p.consumers)if(consumer.owner&&(!consumer.admitted||consumer.admitted(consumer.owner))){
                    any=true;all&=consumer.published&&consumer.published(consumer.owner,picture);
                }
                if(needsCoverage&&(!any||!all))continue;
                auto receipt=picture.metadata;receipt.kind=local::Kind::Ready;receipt.certifiedMetadata.clear();
                if(p.channel.enqueue(receipt)){p.readyQueued[n]=true;count(p,Flow::ReadyQueued);}
            }
            p.channel.tick();
        }
        reportFlowCold(p);
    }catch(...){/* Optional worker loss cannot stop native rendering. */}
}
void work(Process* pointer) noexcept{
    local::State previous=local::State::Off;
    for(;;){
        workCycle(*pointer,previous);
        // Poll is confined to this cold local worker. Data arrival and producer
        // callbacks wake it immediately instead of adding two 2-ms sleeps.
        pollfd descriptors[2]{};
        {std::lock_guard<std::mutex> lock(pointer->gate);descriptors[0]={pointer->channel.activityFd(),short(POLLIN|(pointer->channel.pendingOutput()?POLLOUT:0)),0};descriptors[1]={pointer->wake,POLLIN,0};}
        const auto ignored=poll(descriptors,2,2);(void)ignored;
        if(descriptors[1].revents&POLLIN){std::uint64_t value=0;const auto drained=read(pointer->wake,&value,sizeof value);(void)drained;}
    }
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
        p.wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);if(p.wake<0)return;
        p.kind.store(kind,std::memory_order_release);
        if(kind==ProcessKind::Vrlink){
            if(!p.writer.prepareCold()){p.kind=ProcessKind::Off;return;}
            native::RuntimeRequest request;request.explicitlyEnabled=true;request.buildId=712;request.pairingsDirectory=p.root/"pairings";std::copy_n(address,std::strlen(address)+1,request.producerAddress.data());request.producerPort=std::uint16_t(port);
            request.callbacks={certified,decoded,nullptr,sourceNotice,&p};request.imageRoutePrepared=imageRoutePrepared;request.imageRouteContext=&p;
            if(!native::startPinnedReceiver(request)||!native::setPinnedCaptureSink(local::LeaseWriter::sink,&p.writer)||!native::setPinnedCaptureHandoff(handoff,&p)){native::stopPinnedReceiver();p.kind=ProcessKind::Off;return;}
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
__attribute__((constructor)) static void prepareBeforeMain(){prepareProcessCold();}
} // namespace stream_bridge_renderer
