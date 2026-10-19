#include "stream_bridge_native_runtime.h"
#include "stream_bridge_native_loader.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <bitset>
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
const char* runtimeReasonName(RuntimeReason reason) noexcept{
    constexpr const char* names[]{"none","native-source-changed","image-route-lost","control-transport-lost","control-refused",
        "known-snapshot-gap","known-copy-gap","unknown-snapshot-loss","unknown-copy-loss","capture-events-lost",
        "invalid-gap","native-source-retired","capture-pool-reset","invalid-capture","invalid-input",
        "duplicate-input-token","certified-pending-capacity","quarantine-capacity","invalid-certificate",
        "send-receipt-mismatch","outbound-proof-refused","random-refused","generation-exhausted","exception"};
    const auto index=static_cast<std::size_t>(reason);return index<sizeof(names)/sizeof(*names)?names[index]:"unknown";
}
struct ReceiverWorker::Impl {
    struct Pending {bool used=false,certified=false;Observation submitted{};stream_bridge::FrameDescriptor descriptor{};};
    struct Sending {bool used=false,echo=false;std::uint64_t ticket=0;stream_bridge::Message message{};stream_bridge::ProbeMessage probe{};};
    RuntimeRequest request{};SourceProvider provider{};SourceIdentity source{};
    RuntimeNotice current{};bool enabled=false,blocked=false;SnapshotStats lastStats{};
    // Transport readiness is not source proof. A late listener can recover on a
    // fresh successful QBUF, at a bounded rate, without admitting any old AU.
    bool reconnect=false;std::uint64_t retryAt=0;std::uint32_t retryDelay=250;
    // Uncertified input can produce zero, one or multiple CAPTURE results.
    // Therefore no one completion can safely release its 16-bit token. Keep
    // exact hazards until a real source pool changes; never evict or guess.
    struct Quarantine {int fd=-1;std::uint64_t pool=0;bool hard=false;std::bitset<65536> tokens{};};
    std::array<Quarantine,MaxDevices> quarantines{};
        struct RetiredPool {int fd=-1;std::uint64_t throughPool=0;};
    std::array<RetiredPool,ReceiverWorker::RetiredPoolCapacity> retiredPools{};
    std::uint64_t coveredDropped=0,coveredRejected=0;bool unscopedLoss=false;
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
    bool retired(int fd,std::uint64_t pool) const{
        if(!pool)return true;
        for(const auto& old:retiredPools)if(old.fd==fd&&pool<=old.throughPool)return true;
        for(const auto& q:quarantines)if(q.fd==fd&&pool<q.pool)return true;
        return false;
    }
    Quarantine* quarantine(int fd,std::uint64_t pool){
        if(fd<0||retired(fd,pool))return nullptr;
        Quarantine* empty=nullptr;
        for(auto& q:quarantines){
            if(q.fd==fd){if(q.pool!=pool){q.tokens.reset();q.pool=pool;q.hard=unscopedLoss;}return &q;}
            if(q.fd<0&&!empty)empty=&q;
        }
        if(empty){empty->fd=fd;empty->pool=pool;empty->hard=unscopedLoss;}return empty;
    }
    bool retire(int fd,std::uint64_t pool,std::uint32_t token){
        if(token>0xffff)return false;
        auto* q=quarantine(fd,pool);if(!q)return false;q->tokens.set(token);return true;
    }
    void unknownLoss(){
        // No trustworthy source/token exists for an unknown loss. Neither a
        // route toggle nor an unrelated pool may erase that ambiguity. Only a
        // complete worker restart (before native creation) clears this fence.
        unscopedLoss=true;for(auto& q:quarantines)if(q.fd>=0)q.hard=true;
    }
    bool retirePending(){
        for(const auto& p:pending)if(p.used&&!retire(p.submitted.fd,p.submitted.poolGeneration,p.submitted.nativeTimestampToken))return false;
        return true;
    }
    void notice(RuntimeState state,stream_bridge::ControlStatus controlStatus,RuntimeReason reason=RuntimeReason::None){
        if(current.state==state&&current.control==controlStatus&&current.reason==reason&&current.source.stream.connection==source.stream.connection
            &&current.source.stream.generation==source.stream.generation&&current.source.decoderFd==source.decoderFd)return;
        current={state,source,controlStatus,reason,lastStats};
        if(request.callbacks.notice)request.callbacks.notice(current,request.callbacks.context);
    }
    void notice(RuntimeState state,RuntimeReason reason=RuntimeReason::None){notice(state,control.status(),reason);}
    void clear(RuntimeState state,bool keepSource,RuntimeReason reason=RuntimeReason::None){
        const auto observed=state==RuntimeState::Disabled?stream_bridge::ControlStatus::Disabled:control.status();
        if(keepSource&&reason!=RuntimeReason::TransportLost&&reason!=RuntimeReason::SnapshotGap
            &&reason!=RuntimeReason::CopyGap&&reason!=RuntimeReason::CapturePoolReset){
            for(auto& q:quarantines)if(q.fd==source.decoderFd&&q.pool==source.outputPoolGeneration)q.hard=true;
        }
        receiver.end();control.stop();pending={};sending={};authority={};blocked=keepSource;
        reconnect=false;retryAt=0;
        notice(state,observed,reason);if(!keepSource)source={};
    }
    bool retryTransport(RuntimeReason reason=RuntimeReason::TransportLost,RuntimeState state=RuntimeState::SenderUnavailable){
        if(!retirePending()){unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);return false;}
        clear(state,true,reason);
        const auto clock=now();retryAt=clock>std::numeric_limits<std::uint64_t>::max()-retryDelay
            ?std::numeric_limits<std::uint64_t>::max():clock+retryDelay;
        retryDelay=std::min<std::uint32_t>(2000,retryDelay*2);reconnect=true;return true;
    }
    bool begin(const Observation& fact){
        if(!fact.poolGeneration||fact.fd<0||!fact.bufferCapacity||fact.usedBytes>fact.bufferCapacity)return false;
        if(!matches(fact.fd,fact.poolGeneration))retryDelay=250;
        // A different live decoder does not retire the previous pool. Preserve
        // all old exact native tokens before dropping that session's receipts.
        if(!retirePending()){unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);return false;}
        clear(RuntimeState::SourceLost,false,RuntimeReason::NativeSourceChanged);
        if(admissionGeneration==~std::uint64_t(0)){blocked=true;notice(RuntimeState::SourceRefused,RuntimeReason::GenerationExhausted);return false;}
        admissionGeneration=std::max(admissionGeneration+1,fact.poolGeneration);
        source.decoderFd=fact.fd;source.outputPoolGeneration=fact.poolGeneration;source.stream.generation=admissionGeneration;
        if(!provider.random(source.stream.connection.data(),source.stream.connection.size(),provider.context)||!nonzero(source.stream.connection)){
            blocked=true;notice(RuntimeState::SourceRefused,RuntimeReason::RandomRefused);return false;}
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
            clear(status==stream_bridge::ControlStatus::PairingRefused?RuntimeState::PairingRefused:RuntimeState::SenderUnavailable,true,RuntimeReason::ControlRefused);return false;}
        blocked=false;notice(RuntimeState::Negotiating);return true;
    }
    bool matches(int fd,std::uint64_t pool) const {return source.decoderFd==fd&&source.outputPoolGeneration==pool&&pool!=0;}
    void capture(const CaptureObservation& event){
        if(event.kind==CaptureKind::InputGap){
            auto* covered=event.inputLoss==InputLoss::SnapshotFull?&coveredDropped:
                event.inputLoss==InputLoss::CopyRejected?&coveredRejected:nullptr;
            if(!covered||!event.lossSequence||event.fd<0||!event.sourcePoolGeneration||event.nativeTimestampToken>0xffff){
                unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::InvalidGap);return;
            }
            if(event.lossSequence>*covered){
                if(*covered==~std::uint64_t(0)||event.lossSequence!=*covered+1){
                    unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::InvalidGap);return;}
                *covered=event.lossSequence;
            }
            if(retired(event.fd,event.sourcePoolGeneration))return;
            if(!retire(event.fd,event.sourcePoolGeneration,event.nativeTimestampToken)){
                unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);return;}
            if(matches(event.fd,event.sourcePoolGeneration)&&!blocked)
                retryTransport(event.inputLoss==InputLoss::SnapshotFull?RuntimeReason::SnapshotGap:RuntimeReason::CopyGap,RuntimeState::SourceLost);
            return;
        }
        if(event.kind==CaptureKind::SourceReset&&event.fd>=0&&event.sourcePoolGeneration){
            // Retire only this fd through its observed monotonic pool epoch.
            // Another live fd may have an older global generation and keeps
            // every token hazard. Bounded per-fd tombstones allow reuse without
            // permitting delayed events to recreate an old quarantine.
            RetiredPool* record=nullptr;RetiredPool* free=nullptr;
            for(auto& old:retiredPools){
                if(old.fd==event.fd)record=&old;
                if(old.fd<0&&!free)free=&old;
            }
            if(!record){
                if(!free){unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);return;}
                record=free;record->fd=event.fd;
            }
            record->throughPool=std::max(record->throughPool,event.sourcePoolGeneration);
            for(auto& q:quarantines)if(q.fd==event.fd&&q.pool<=event.sourcePoolGeneration)q=Quarantine{};
            if(source.decoderFd==event.fd&&source.outputPoolGeneration&&source.outputPoolGeneration<=event.sourcePoolGeneration){
                if(request.callbacks.capture)request.callbacks.capture(source,event,request.callbacks.context);
                clear(RuntimeState::SourceLost,true,RuntimeReason::NativeSourceRetired);
            }
            return;
        }
        if(!matches(event.fd,event.sourcePoolGeneration))return;
        if(request.callbacks.capture)request.callbacks.capture(source,event,request.callbacks.context);
        if(event.kind==CaptureKind::Reset){
            if(!blocked)retryTransport(RuntimeReason::CapturePoolReset,RuntimeState::SourceLost);
            return;
        }
        if(event.kind!=CaptureKind::Decoded||blocked)return;
        const auto* q=quarantine(event.fd,event.sourcePoolGeneration);
        if(!q){unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);return;}
        if(event.nativeTimestampToken>0xffff||q->tokens.test(event.nativeTimestampToken))return;
        Pending* found=nullptr;unsigned matches=0;
        for(auto& p:pending)if(p.used&&p.submitted.nativeTimestampToken==event.nativeTimestampToken){found=&p;++matches;}
        if(matches!=1||!found)return;
        if(event.dmabufFd<0||!event.capturePoolGeneration||!event.contentGeneration){clear(RuntimeState::SourceLost,true,RuntimeReason::InvalidCapture);return;}
        const DecodedFrame frame{source,found->descriptor,found->submitted,event,receiver.snapshot().peerConfirmed};
        if(request.callbacks.decoded)request.callbacks.decoded(frame,request.callbacks.context);
        found->used=false;
    }
    void input(const Observation& fact){
        if(fact.fd<0||!fact.poolGeneration||fact.nativeTimestampToken>0xffff||!fact.contentGeneration
            ||!fact.bufferCapacity||fact.usedBytes==0||fact.usedBytes>bytes.size()||fact.usedBytes>fact.bufferCapacity){
            clear(RuntimeState::SourceLost,true,RuntimeReason::InvalidInput);return;}
        if(retired(fact.fd,fact.poolGeneration))return;
        auto* q=quarantine(fact.fd,fact.poolGeneration);
        if(!q){unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);return;}
        if(q->hard){q->tokens.set(fact.nativeTimestampToken);return;}
        if(!matches(fact.fd,fact.poolGeneration)){if(!begin(fact)){q->tokens.set(fact.nativeTimestampToken);return;}}
        else if(blocked){
            if(!reconnect||now()<retryAt){q->tokens.set(fact.nativeTimestampToken);return;}
            if(!begin(fact)){q->tokens.set(fact.nativeTimestampToken);return;}
        }
        // A live duplicate is ambiguous even if the second input is ordinary.
        Pending* slot=nullptr;
        for(auto& p:pending){if(p.used&&p.submitted.nativeTimestampToken==fact.nativeTimestampToken){clear(RuntimeState::SourceLost,true,RuntimeReason::DuplicateInputToken);return;}
            if(!p.used&&!slot)slot=&p;}
        if(q->tokens.test(fact.nativeTimestampToken))return;
        stream_bridge_receiver::AccessUnitContext context{};context.authority=authority;
        context.native={static_cast<std::uint16_t>(fact.nativeTimestampToken),fact.contentGeneration};
        context.sourceBoundaryVerified=true; // Pinned provider's owned successful QBUF copy.
        stream_bridge::CertifiedAccessUnit certificate{};
        const stream_bridge::Bytes observed{bytes.data(),fact.usedBytes};
        // Ordinary metadata/chunks need not ever produce a CAPTURE. They have
        // no certified picture receipt to retain in the bounded Pending table.
        if(!control.verifyAccessUnit(observed,context,certificate)){q->tokens.set(fact.nativeTimestampToken);return;}
        if(!slot){clear(RuntimeState::SourceLost,true,RuntimeReason::PendingCapacity);return;}
        context.completeAnnexBAccessUnit=true;
        const auto result=receiver.observeAccessUnit({certificate.original.data(),certificate.original.size()},context);
        using R=stream_bridge_receiver::Result;
        if(result.result==R::InvalidOwned||result.result==R::WrongSession||result.result==R::Incompatible
            ||result.result==R::EvidenceLost||result.result==R::CapacityExceeded){clear(RuntimeState::SourceLost,true,RuntimeReason::InvalidCertificate);return;}
        stream_bridge::LocalConfig local{};stream_bridge::LocalSendAuthority localAuthority{};
        if(control.localConfig(local,localAuthority))authority.channel=localAuthority.channel;
        *slot={true,true,fact,certificate.descriptor};
        const CertifiedFrame frame{source,fact,&certificate,receiver.snapshot().peerConfirmed,observed};
        if(request.callbacks.certified)request.callbacks.certified(frame,request.callbacks.context);
    }
    void collect(){
        // Read counters BEFORE their ordered post-success gap queue. A later
        // publisher may be covered early; it cannot create a false missing gap.
        const auto stats=provider.stats(provider.context),previous=lastStats;lastStats=stats;
        CaptureObservation event{};
        for(unsigned i=0;i<64&&provider.capture(event,provider.context);++i)capture(event);
        RuntimeReason loss=RuntimeReason::None;
        if(stats.captureDropped!=previous.captureDropped)loss=RuntimeReason::CaptureEventsLost;
        else if(stats.dropped>coveredDropped||stats.dropped<previous.dropped)loss=RuntimeReason::UnknownSnapshotLoss;
        else if(stats.rejected>coveredRejected||stats.rejected<previous.rejected)loss=RuntimeReason::UnknownCopyLoss;
        if(loss!=RuntimeReason::None){unknownLoss();clear(RuntimeState::SourceLost,true,loss);}
        coveredDropped=std::max(coveredDropped,stats.dropped);coveredRejected=std::max(coveredRejected,stats.rejected);
    }
    void discardInputs(){
        Observation discarded{};
        for(unsigned i=0;i<SnapshotCapacity&&provider.poll(discarded,bytes.data(),bytes.size(),provider.context);++i)
            if(!retired(discarded.fd,discarded.poolGeneration)&&!retire(discarded.fd,discarded.poolGeneration,discarded.nativeTimestampToken)){
                unknownLoss();clear(RuntimeState::SourceLost,true,RuntimeReason::InvalidInput);
            }
    }
    void controlTick(){
        if(blocked||!nonzero(source.stream.connection))return;
        control.tick();
        if(control.status()==stream_bridge::ControlStatus::IoRefused||control.status()==stream_bridge::ControlStatus::Lost){retryTransport();return;}
        if(terminal(control.status())){clear(RuntimeState::SenderUnavailable,true,RuntimeReason::ControlRefused);return;}
        stream_bridge::SendReceipt sent{};
        while(control.pollSent(sent)){
            if(!sending.used||sent.ticket!=sending.ticket){clear(RuntimeState::SourceLost,true,RuntimeReason::SendReceiptMismatch);return;}
            if(sending.echo){if(!receiver.markEchoSent(sending.probe,authority)){clear(RuntimeState::SourceLost,true,RuntimeReason::OutboundProofRefused);return;}}
            else if(receiver.markOutboundSent(sending.message,authority)!=stream_bridge::Status::Ok){clear(RuntimeState::SourceLost,true,RuntimeReason::OutboundProofRefused);return;}
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
        impl_->coveredDropped=impl_->lastStats.dropped;impl_->coveredRejected=impl_->lastStats.rejected;
        impl_->notice(RuntimeState::WaitingNativeSource);return true;}catch(...){stop();return false;}
}
void ReceiverWorker::tick() noexcept{
    if(!impl_->enabled)return;
    try{
        const bool ready=impl_->request.imageRoutePrepared&&impl_->request.imageRoutePrepared(impl_->request.imageRouteContext);
        if(!ready){
            if(impl_->routeWasReady){
                if(!impl_->retirePending()){impl_->unknownLoss();impl_->clear(RuntimeState::SourceLost,true,RuntimeReason::QuarantineCapacity);}
                else impl_->clear(RuntimeState::SourceLost,false,RuntimeReason::ImageRouteLost);
            }
            impl_->routeSeen=true;impl_->routeWasReady=false;
            impl_->collect();impl_->discardInputs();impl_->collect();
            impl_->notice(RuntimeState::WaitingImageRoute);return;
        }
        if(impl_->routeSeen&&!impl_->routeWasReady){
            impl_->collect();impl_->discardInputs();impl_->collect();
            impl_->routeWasReady=true;impl_->notice(RuntimeState::WaitingNativeSource);return;
        }
        impl_->routeSeen=impl_->routeWasReady=true;
        impl_->collect();
        Observation fact{};
        for(unsigned i=0;i<SnapshotCapacity&&impl_->provider.poll(fact,impl_->bytes.data(),impl_->bytes.size(),impl_->provider.context);++i)impl_->input(fact);
        impl_->collect();
        impl_->controlTick();
    }catch(...){impl_->clear(RuntimeState::SourceLost,true,RuntimeReason::Exception);}
}
void ReceiverWorker::stop() noexcept{
    if(!impl_)return;
    impl_->clear(RuntimeState::Disabled,false);impl_->enabled=false;impl_->request={};impl_->provider={};
    impl_->quarantines={};impl_->retiredPools={};impl_->coveredDropped=impl_->coveredRejected=0;impl_->unscopedLoss=false;impl_->retryDelay=250;
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
