#include "stream_bridge_native_runtime.h"
#include "stream_bridge_native_loader.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
namespace stream_bridge_native {
namespace {
bool nonzero(const stream_bridge::Nonce& n){return std::any_of(n.begin(),n.end(),[](auto b){return b!=0;});}
bool terminal(stream_bridge::ControlStatus s){using S=stream_bridge::ControlStatus;return s==S::Lost||s==S::IoRefused
    ||s==S::AuthenticationRefused||s==S::CapacityRefused||s==S::PairingRefused||s==S::AddressRefused||s==S::RandomRefused;}
}
struct ReceiverWorker::Impl {
    struct Pending {bool used=false,certified=false;Observation submitted{};stream_bridge::FrameDescriptor descriptor{};};
    struct Sending {bool used=false,echo=false;std::uint64_t ticket=0;stream_bridge::Message message{};stream_bridge::ProbeMessage probe{};};
    RuntimeRequest request{};SourceProvider provider{};SourceIdentity source{};
    RuntimeNotice current{};bool enabled=false,blocked=false;SnapshotStats lastStats{};
    // Transport readiness is not source proof. A late listener can recover on a
    // fresh successful QBUF, at a bounded rate, without admitting any old AU.
    bool reconnect=false;std::uint64_t retryAt=0;std::uint32_t retryDelay=250;
    struct RetiredToken {bool used=false;std::uint32_t token=0;};
    std::array<RetiredToken,ReceiverWorker::PendingCapacity> retired{};
    bool routeSeen=false,routeWasReady=false;std::uint64_t admissionGeneration=0;
    stream_bridge::ControlRuntime control;stream_bridge_receiver::Receiver receiver;
    stream_bridge_receiver::Authority authority{};
    std::array<Pending,ReceiverWorker::PendingCapacity> pending{};Sending sending{};
    std::array<std::uint8_t,MaxCodedBytes> bytes{};
    std::uint64_t now()const{
        if(provider.monotonicMilliseconds)return provider.monotonicMilliseconds(provider.context);
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void notice(RuntimeState state,stream_bridge::ControlStatus controlStatus){
        if(current.state==state&&current.control==controlStatus&&current.source.stream.connection==source.stream.connection
            &&current.source.stream.generation==source.stream.generation&&current.source.decoderFd==source.decoderFd)return;
        current={state,source,controlStatus};
        if(request.callbacks.notice)request.callbacks.notice(current,request.callbacks.context);
    }
    void notice(RuntimeState state){notice(state,control.status());}
    void clear(RuntimeState state,bool keepSource){
        const auto observed=state==RuntimeState::Disabled?stream_bridge::ControlStatus::Disabled:control.status();
        receiver.end();control.stop();pending={};sending={};authority={};blocked=keepSource;
        reconnect=false;retryAt=0;
        notice(state,observed);if(!keepSource)source={};
    }
    bool retryTransport(){
        // A late capture may still describe an old pending token. Quarantine
        // it until that exact native completion arrives; never join it to a
        // new admission which happens to reuse the same 16-bit decoder token.
        for(const auto& p:pending)if(p.used){
            bool recorded=false;for(const auto& t:retired)recorded|=t.used&&t.token==p.submitted.nativeTimestampToken;
            if(recorded)continue;
            auto* free=static_cast<RetiredToken*>(nullptr);for(auto& t:retired)if(!t.used){free=&t;break;}
            if(!free){clear(RuntimeState::SourceLost,true);return false;}
            *free={true,p.submitted.nativeTimestampToken};
        }
        clear(RuntimeState::SenderUnavailable,true);
        const auto clock=now();retryAt=clock>std::numeric_limits<std::uint64_t>::max()-retryDelay
            ?std::numeric_limits<std::uint64_t>::max():clock+retryDelay;
        retryDelay=std::min<std::uint32_t>(2000,retryDelay*2);reconnect=true;return true;
    }
    bool begin(const Observation& fact){
        if(!fact.poolGeneration||fact.fd<0||!fact.bufferCapacity||fact.usedBytes>fact.bufferCapacity)return false;
        if(!matches(fact.fd,fact.poolGeneration)){retired={};retryDelay=250;}
        clear(RuntimeState::SourceLost,false);
        if(admissionGeneration==~std::uint64_t(0)){blocked=true;notice(RuntimeState::SourceRefused);return false;}
        admissionGeneration=std::max(admissionGeneration+1,fact.poolGeneration);
        source.decoderFd=fact.fd;source.outputPoolGeneration=fact.poolGeneration;source.stream.generation=admissionGeneration;
        if(!provider.random(source.stream.connection.data(),source.stream.connection.size(),provider.context)||!nonzero(source.stream.connection)){
            blocked=true;notice(RuntimeState::SourceRefused);return false;}
        stream_bridge::ControlRequest config{};config.explicitlyEnabled=true;config.role=stream_bridge::Role::Receiver;
        config.offered=config.required=stream_bridge::KnownCapabilities;
        config.nativeAccessUnitCapacity=static_cast<std::uint32_t>(std::min<std::size_t>(fact.bufferCapacity,MaxCodedBytes));
        config.buildId=request.buildId;config.pairingsDirectory=request.pairingsDirectory;
        config.address=request.producerAddress;config.port=request.producerPort;config.video=source.stream;
        const auto status=control.prepare(config);
        stream_bridge::LocalConfig local{};
        authority.session=source.stream.connection;authority.streamGeneration=source.stream.generation;
        if(status==stream_bridge::ControlStatus::IoRefused||status==stream_bridge::ControlStatus::Lost){retryTransport();return false;}
        if(terminal(status)||!control.configuredLocal(local)||!receiver.beginAuthenticated(authority,local,control.receiverVerifier(),control.verifierContext())){
            clear(status==stream_bridge::ControlStatus::PairingRefused?RuntimeState::PairingRefused:RuntimeState::SenderUnavailable,true);return false;}
        blocked=false;notice(RuntimeState::Negotiating);return true;
    }
    bool matches(int fd,std::uint64_t pool) const {return source.decoderFd==fd&&source.outputPoolGeneration==pool&&pool!=0;}
    void capture(const CaptureObservation& event){
        if(!matches(event.fd,event.sourcePoolGeneration))return;
        if(request.callbacks.capture)request.callbacks.capture(source,event,request.callbacks.context);
        if(event.kind==CaptureKind::SourceReset){clear(RuntimeState::SourceLost,true);return;}
        if(event.kind==CaptureKind::Reset){pending={};return;}
        if(event.kind!=CaptureKind::Decoded)return;
        for(auto& t:retired)if(t.used&&t.token==event.nativeTimestampToken){t={};return;}
        if(blocked)return;
        Pending* found=nullptr;unsigned matches=0;
        for(auto& p:pending)if(p.used&&p.submitted.nativeTimestampToken==event.nativeTimestampToken){found=&p;++matches;}
        if(matches!=1||!found)return;
        if(event.dmabufFd<0||!event.capturePoolGeneration||!event.contentGeneration){clear(RuntimeState::SourceLost,true);return;}
        if(!found->certified){found->used=false;return;}
        const DecodedFrame frame{source,found->descriptor,found->submitted,event,receiver.snapshot().peerConfirmed};
        if(request.callbacks.decoded)request.callbacks.decoded(frame,request.callbacks.context);
        found->used=false;
    }
    void input(const Observation& fact){
        if(!matches(fact.fd,fact.poolGeneration)){if(!begin(fact))return;}
        else if(blocked){if(!reconnect||now()<retryAt||!begin(fact))return;}
        for(const auto& t:retired)if(t.used&&t.token==fact.nativeTimestampToken)return;
        if(fact.nativeTimestampToken>0xffff||!fact.contentGeneration||fact.usedBytes==0||fact.usedBytes>bytes.size()){
            clear(RuntimeState::SourceLost,true);return;}
        // A pending duplicate is ambiguous even if it carries different crypto
        // metadata. Never select a newest entry for a reused native token.
        Pending* slot=nullptr;
        for(auto& p:pending){if(p.used&&p.submitted.nativeTimestampToken==fact.nativeTimestampToken){clear(RuntimeState::SourceLost,true);return;}
            if(!p.used&&!slot)slot=&p;}
        if(!slot){clear(RuntimeState::SourceLost,true);return;}
        slot->used=true;slot->submitted=fact;
        stream_bridge_receiver::AccessUnitContext context{};context.authority=authority;
        context.native={static_cast<std::uint16_t>(fact.nativeTimestampToken),fact.contentGeneration};
        context.sourceBoundaryVerified=true; // This input is the pinned provider's owned successful QBUF copy.
        stream_bridge::CertifiedAccessUnit certificate{};
        const stream_bridge::Bytes observed{bytes.data(),fact.usedBytes};
        // Source completion is not HEVC completeness. Only the exact paired
        // original-AU size/hash/certificate check can establish this assertion.
        if(!control.verifyAccessUnit(observed,context,certificate))return;
        context.completeAnnexBAccessUnit=true;
        const auto result=receiver.observeAccessUnit({certificate.original.data(),certificate.original.size()},context);
        using R=stream_bridge_receiver::Result;
        if(result.result==R::InvalidOwned||result.result==R::WrongSession||result.result==R::Incompatible
            ||result.result==R::EvidenceLost||result.result==R::CapacityExceeded){clear(RuntimeState::SourceLost,true);return;}
        stream_bridge::LocalConfig local{};stream_bridge::LocalSendAuthority localAuthority{};
        if(control.localConfig(local,localAuthority))authority.channel=localAuthority.channel;
        slot->certified=true;slot->descriptor=certificate.descriptor;
        const CertifiedFrame frame{source,fact,&certificate,receiver.snapshot().peerConfirmed,observed};
        if(request.callbacks.certified)request.callbacks.certified(frame,request.callbacks.context);
    }
    void controlTick(){
        if(blocked||!nonzero(source.stream.connection))return;
        control.tick();
        if(control.status()==stream_bridge::ControlStatus::IoRefused||control.status()==stream_bridge::ControlStatus::Lost){retryTransport();return;}
        if(terminal(control.status())){clear(RuntimeState::SenderUnavailable,true);return;}
        stream_bridge::SendReceipt sent{};
        while(control.pollSent(sent)){
            if(!sending.used||sent.ticket!=sending.ticket){clear(RuntimeState::SourceLost,true);return;}
            if(sending.echo){if(!receiver.markEchoSent(sending.probe,authority)){clear(RuntimeState::SourceLost,true);return;}}
            else if(receiver.markOutboundSent(sending.message,authority)!=stream_bridge::Status::Ok){clear(RuntimeState::SourceLost,true);return;}
            sending={};
        }
        if(!sending.used){
            stream_bridge::AuthPayload plain{};bool ready=false;Sending next{};
            if(receiver.nextOutbound(next.message)==stream_bridge::Status::Ok){ready=stream_bridge::encode(next.message,plain)==stream_bridge::Status::Ok;}
            else if(receiver.nextEcho(next.probe)){next.echo=true;ready=stream_bridge::encodeProbe(next.probe,plain)==stream_bridge::Status::Ok;}
            if(ready&&control.queueControl({plain.data(),plain.size()},next.ticket)){next.used=true;sending=next;}
        }
        if(receiver.snapshot().peerConfirmed)retryDelay=250;
        notice(receiver.snapshot().peerConfirmed?RuntimeState::PeerConfirmed:RuntimeState::Negotiating);
    }
};
ReceiverWorker::ReceiverWorker():impl_(new Impl){}
ReceiverWorker::~ReceiverWorker(){stop();}
bool ReceiverWorker::prepare(const RuntimeRequest& request,SourceProvider provider) noexcept{
    stop();if(!request.explicitlyEnabled)return true;
    if(!request.buildId||request.pairingsDirectory.empty()||!request.producerPort
        ||!provider.poll||!provider.capture||!provider.stats||!provider.random)return false;
    try{impl_->request=request;impl_->provider=provider;impl_->enabled=true;impl_->lastStats=provider.stats(provider.context);
        impl_->notice(RuntimeState::WaitingNativeSource);return true;}catch(...){stop();return false;}
}
void ReceiverWorker::tick() noexcept{
    if(!impl_->enabled)return;
    try{
        const bool ready=impl_->request.imageRoutePrepared&&impl_->request.imageRoutePrepared(impl_->request.imageRouteContext);
        if(!ready){
            if(impl_->routeWasReady)impl_->clear(RuntimeState::SourceLost,false);
            impl_->routeSeen=true;impl_->routeWasReady=false;
            CaptureObservation discarded{};for(unsigned i=0;i<64&&impl_->provider.capture(discarded,impl_->provider.context);++i){}
            impl_->lastStats=impl_->provider.stats(impl_->provider.context);impl_->notice(RuntimeState::WaitingImageRoute);return;
        }
        if(impl_->routeSeen&&!impl_->routeWasReady){
            Observation discarded{};for(unsigned i=0;i<SnapshotCapacity&&impl_->provider.poll(discarded,impl_->bytes.data(),impl_->bytes.size(),impl_->provider.context);++i){}
            CaptureObservation retired{};for(unsigned i=0;i<64&&impl_->provider.capture(retired,impl_->provider.context);++i){}
            impl_->lastStats=impl_->provider.stats(impl_->provider.context);impl_->routeWasReady=true;impl_->notice(RuntimeState::WaitingNativeSource);return;
        }
        impl_->routeSeen=impl_->routeWasReady=true;
        const auto stats=impl_->provider.stats(impl_->provider.context);
        if(stats.dropped!=impl_->lastStats.dropped||stats.rejected!=impl_->lastStats.rejected||stats.captureDropped!=impl_->lastStats.captureDropped){
            impl_->clear(RuntimeState::SourceLost,true);}
        impl_->lastStats=stats;
        CaptureObservation event{};
        for(unsigned i=0;i<64&&impl_->provider.capture(event,impl_->provider.context);++i)impl_->capture(event);
        Observation fact{};
        for(unsigned i=0;i<SnapshotCapacity&&impl_->provider.poll(fact,impl_->bytes.data(),impl_->bytes.size(),impl_->provider.context);++i)impl_->input(fact);
        for(unsigned i=0;i<64&&impl_->provider.capture(event,impl_->provider.context);++i)impl_->capture(event);
        impl_->controlTick();
    }catch(...){impl_->clear(RuntimeState::SourceLost,true);}
}
void ReceiverWorker::stop() noexcept{
    if(!impl_)return;
    impl_->clear(RuntimeState::Disabled,false);impl_->enabled=false;impl_->request={};impl_->provider={};
    impl_->retired={};impl_->retryDelay=250;
    impl_->routeSeen=impl_->routeWasReady=false;
}
RuntimeNotice ReceiverWorker::status()const noexcept{return impl_->current;}
namespace {
std::mutex lifetime;std::unique_ptr<ReceiverWorker> worker;std::thread thread;
std::atomic<bool> running{false};std::condition_variable wake;std::mutex sleepMutex;
struct StopAtExit {~StopAtExit(){stopPinnedReceiver();}} stopAtExit;
bool poll(Observation& out,std::uint8_t* bytes,std::size_t n,void*) noexcept{return pollPinnedProcess(out,bytes,n);}
bool capture(CaptureObservation& out,void*) noexcept{return pollPinnedCapture(out);}
SnapshotStats stats(void*) noexcept{return pinnedSnapshotStats();}
bool random(void* out,std::size_t n,void*) noexcept{return stream_bridge::bridgeSystemRandom(out,n);}
}
bool startPinnedReceiver(const RuntimeRequest& request) noexcept{
    if(!request.explicitlyEnabled)return true;
    std::lock_guard<std::mutex> lock(lifetime);if(running.load(std::memory_order_acquire))return false;
    try{
        auto candidate=std::make_unique<ReceiverWorker>();
        if(!candidate->prepare(request,{poll,capture,stats,random,nullptr})||!publishPinnedDecodeImports())return false;
        worker=std::move(candidate);running.store(true,std::memory_order_release);
        try{thread=std::thread([]{while(running.load(std::memory_order_acquire)){
            worker->tick();std::unique_lock<std::mutex> lock(sleepMutex);
            wake.wait_for(lock,std::chrono::milliseconds(2),[]{return !running.load(std::memory_order_acquire);});}
            worker->stop();});}
        catch(...){running.store(false,std::memory_order_release);worker.reset();restorePinnedDecodeImports();return false;}
        return true;
    }catch(...){return false;}
}
void stopPinnedReceiver() noexcept{
    std::lock_guard<std::mutex> lock(lifetime);running.store(false,std::memory_order_release);wake.notify_all();
    if(thread.joinable()){thread.join();}
    worker.reset();restorePinnedDecodeImports();
}
bool pinnedReceiverRunning() noexcept{return running.load(std::memory_order_acquire);}
}
