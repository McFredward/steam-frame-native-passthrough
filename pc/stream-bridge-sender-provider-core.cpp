#include "stream-bridge-sender-provider-core.h"
#include <cstring>
#include <limits>

namespace stream_bridge::sender {
static_assert(NVENCAPI_MAJOR_VERSION==11&&NVENCAPI_MINOR_VERSION==1,"SDK 11.1 only");
static_assert(sizeof(void*)==8&&sizeof(NV_ENCODE_API_FUNCTION_LIST)==0x9f8,"64-bit table ABI");
static_assert(NV_ENCODE_API_FUNCTION_LIST_VER==0x7102000b,"frozen table version");
static_assert(sizeof(NV_ENC_LOCK_BITSTREAM)==0x608&&NV_ENC_LOCK_BITSTREAM_VER==0x7101000b,"frozen lock ABI");
static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST,nvEncLockBitstream)==0x90&&
              offsetof(NV_ENCODE_API_FUNCTION_LIST,nvEncUnlockBitstream)==0x98,"source-proven copy/unlock slots");
std::atomic<NvencBoundary*> NvencBoundary::instance_{nullptr};
NvencBoundary::NvencBoundary(const std::array<std::uint8_t,32>& hash):hash_(hash){}
NvencBoundary::Session* NvencBoundary::find(void* e){for(auto& s:sessions_)if(s.encoder==e&&e)return &s;return nullptr;}
std::uint64_t NvencBoundary::revoke(Session& s,bool retainRegistrations){
    s.adapter.stop();s.initialized=false;s.hevc=false;s.encoded=false;
    for(auto& p:s.submissions)if(p.evidence.submission&&inputObserver_.retire)try{inputObserver_.retire(p.evidence,inputObserver_.context);}catch(...){}
    if(!retainRegistrations){s.maps={};s.resources={};}
    s.submissions={};s.probeQueued=false;
    s.generation=nextGeneration_==std::numeric_limits<std::uint64_t>::max()?0:++nextGeneration_;
    return s.generation;
}
VerifiedSource NvencBoundary::source(const Session& s)const {
    VerifiedSource v{};v.verified=installed_&&s.initialized&&s.hevc;v.ordinaryStream=s.hevc;
    v.completeAccessUnit=true;v.codec=VerifiedSource::Codec::HevcAnnexB;
    v.moduleSha256=hash_;v.apiVersion=NV_ENCODE_API_FUNCTION_LIST_VER;v.encoderGeneration=s.generation;return v;
}
NVENCSTATUS NvencBoundary::create(NvencFactory original,NV_ENCODE_API_FUNCTION_LIST* table){
    // The original always owns argument validation and return status.
    const bool requested=table&&table->version==NV_ENCODE_API_FUNCTION_LIST_VER;
    const auto result=original(table);
    if(result!=NV_ENC_SUCCESS||!requested||table->version!=NV_ENCODE_API_FUNCTION_LIST_VER)return result;
    std::lock_guard<std::mutex> guard(mutex_);
    if(!enabled_||(instance_.load(std::memory_order_acquire)&&instance_.load(std::memory_order_acquire)!=this))return result;
    if(!table->nvEncOpenEncodeSessionEx||!table->nvEncInitializeEncoder||!table->nvEncLockBitstream||
       !table->nvEncUnlockBitstream||!table->nvEncDestroyEncoder||!table->nvEncReconfigureEncoder)return result;
    if(installed_&&(table->nvEncOpenEncodeSessionEx!=original_.nvEncOpenEncodeSessionEx||
       table->nvEncInitializeEncoder!=original_.nvEncInitializeEncoder||
       table->nvEncLockBitstream!=original_.nvEncLockBitstream||table->nvEncUnlockBitstream!=original_.nvEncUnlockBitstream||
       table->nvEncDestroyEncoder!=original_.nvEncDestroyEncoder||table->nvEncReconfigureEncoder!=original_.nvEncReconfigureEncoder||
       (inputHooks_&&(table->nvEncRegisterResource!=original_.nvEncRegisterResource||table->nvEncUnregisterResource!=original_.nvEncUnregisterResource||table->nvEncMapInputResource!=original_.nvEncMapInputResource||table->nvEncUnmapInputResource!=original_.nvEncUnmapInputResource||table->nvEncEncodePicture!=original_.nvEncEncodePicture))))return result;
    if(!installed_){original_=*table;instance_.store(this,std::memory_order_release);installed_=true;}
    table->nvEncOpenEncodeSessionEx=open;table->nvEncInitializeEncoder=init;
    table->nvEncLockBitstream=lock;table->nvEncUnlockBitstream=unlock;
    table->nvEncDestroyEncoder=destroy;table->nvEncReconfigureEncoder=reconfigure;
    inputHooks_=original_.nvEncRegisterResource&&original_.nvEncUnregisterResource&&original_.nvEncMapInputResource&&original_.nvEncUnmapInputResource&&original_.nvEncEncodePicture;
    if(inputHooks_){table->nvEncRegisterResource=registerResource;table->nvEncUnregisterResource=unregisterResource;table->nvEncMapInputResource=mapInput;table->nvEncUnmapInputResource=unmapInput;table->nvEncEncodePicture=encode;}
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params,void** encoder){
    auto& self=*instance_.load(std::memory_order_acquire);const auto result=self.original_.nvEncOpenEncodeSessionEx(params,encoder);
    if(result!=NV_ENC_SUCCESS||!encoder||!*encoder)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);
    // Duplicate handles are not relabelled while a previous session still lives.
    if(!self.enabled_||self.find(*encoder)||self.nextGeneration_==std::numeric_limits<std::uint64_t>::max())return result;
    for(auto& s:self.sessions_)if(!s.encoder){s.encoder=*encoder;s.generation=++self.nextGeneration_;
        if(params&&params->version==NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER&&params->device&&self.deviceObserver_.opened)try{self.deviceObserver_.opened(*encoder,s.generation,params->device,params->deviceType,self.deviceObserver_.context);}catch(...){}
        break;}
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::init(void* encoder,NV_ENC_INITIALIZE_PARAMS* params){
    auto& self=*instance_.load(std::memory_order_acquire);const bool admissible=params&&params->version==NV_ENC_INITIALIZE_PARAMS_VER;
    const auto width=admissible?params->encodeWidth:0,height=admissible?params->encodeHeight:0;
    const bool hevc=admissible&&std::memcmp(&params->encodeGUID,&NV_ENC_CODEC_HEVC_GUID,sizeof(GUID))==0&&
        !params->enableOutputInVidmem&&!params->enableMEOnlyMode&&!params->reportSliceOffsets&&!params->enableSubFrameWrite;
    std::uint64_t generation=0;
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))generation=self.revoke(*s);}
    const auto result=self.original_.nvEncInitializeEncoder(encoder,params);
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder);s&&generation&&s->generation==generation){
        s->initialized=result==NV_ENC_SUCCESS&&admissible;s->hevc=s->initialized&&hevc;s->width=width;s->height=height;
    }
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::reconfigure(void* encoder,NV_ENC_RECONFIGURE_PARAMS* params){
    auto& self=*instance_.load(std::memory_order_acquire);
    const bool typed=params&&params->version==NV_ENC_RECONFIGURE_PARAMS_VER
        &&params->reInitEncodeParams.version==NV_ENC_INITIALIZE_PARAMS_VER;
    const auto* init=typed?&params->reInitEncodeParams:nullptr;
    const bool hevc=init&&std::memcmp(&init->encodeGUID,&NV_ENC_CODEC_HEVC_GUID,sizeof(GUID))==0
        &&!init->enableOutputInVidmem&&!init->enableMEOnlyMode&&!init->reportSliceOffsets&&!init->enableSubFrameWrite
        &&init->encodeWidth&&init->encodeHeight&&init->encodeWidth<=16384&&init->encodeHeight<=16384;
    const auto width=hevc?init->encodeWidth:0,height=hevc?init->encodeHeight:0;
    std::uint64_t generation=0;
    // Revoke before calling native code. Native registration/map handles can
    // remain valid after successful Reconfigure, but no submission, handshake,
    // mask, or old metadata identity crosses the generation boundary.
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))generation=self.revoke(*s,hevc);}
    const auto result=self.original_.nvEncReconfigureEncoder(encoder,params);
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder);s&&generation&&s->generation==generation){
        if(self.enabled_&&result==NV_ENC_SUCCESS&&hevc&&s->nextIdentity<=std::numeric_limits<std::uint64_t>::max()-64){
            // Rebase CURRENT records, not a pre-call snapshot: a nested native
            // unregister/unmap must never be resurrected on return.
            for(auto& r:s->resources)if(r.registered)r.generation=++s->nextIdentity;
            for(auto& m:s->maps)if(m.mapped){
                Session::Resource* found=nullptr;for(auto& r:s->resources)if(r.registered==m.registered)found=&r;
                if(found){m.resourceGeneration=found->generation;m.generation=++s->nextIdentity;}else m={};
            }
            s->initialized=s->hevc=true;s->width=width;s->height=height;
        }else{s->maps={};s->resources={};}
    }
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::registerResource(void* encoder,NV_ENC_REGISTER_RESOURCE* params){
    auto& self=*instance_.load(std::memory_order_acquire);
    const bool typed=params&&params->version==NV_ENC_REGISTER_RESOURCE_VER;
    const auto result=self.original_.nvEncRegisterResource(encoder,params);
    if(result!=NV_ENC_SUCCESS||!typed||params->version!=NV_ENC_REGISTER_RESOURCE_VER)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);auto s=self.find(encoder);
    if(!self.enabled_||!s||!params->registeredResource||!params->resourceToRegister||params->resourceType!=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX||params->bufferUsage!=NV_ENC_INPUT_IMAGE||!params->width||!params->height||params->width>16384||params->height>16384||s->nextIdentity==~std::uint64_t(0))return result;
    for(auto& r:s->resources)if(r.registered==params->registeredResource)return result;
    for(auto& r:s->resources)if(!r.registered){r={params->registeredResource,params->resourceToRegister,++s->nextIdentity,params->width,params->height,params->subResourceIndex,params->bufferFormat};break;}
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::unregisterResource(void* encoder,NV_ENC_REGISTERED_PTR resource){
    auto& self=*instance_.load(std::memory_order_acquire);const auto result=self.original_.nvEncUnregisterResource(encoder,resource);
    if(result!=NV_ENC_SUCCESS)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder)){
        for(auto& r:s->resources)if(r.registered==resource)r={};
        for(auto& m:s->maps)if(m.registered==resource)m={};
        for(auto& p:s->submissions)if(!p.locked&&p.evidence.resourceGeneration){bool alive=false;for(auto& r:s->resources)alive|=r.generation==p.evidence.resourceGeneration;if(!alive)p={};}
    }return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::mapInput(void* encoder,NV_ENC_MAP_INPUT_RESOURCE* params){
    auto& self=*instance_.load(std::memory_order_acquire);const bool typed=params&&params->version==NV_ENC_MAP_INPUT_RESOURCE_VER;
    const auto result=self.original_.nvEncMapInputResource(encoder,params);
    if(result!=NV_ENC_SUCCESS||!typed||params->version!=NV_ENC_MAP_INPUT_RESOURCE_VER)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);auto s=self.find(encoder);if(!self.enabled_||!s||!params->mappedResource||s->nextIdentity==~std::uint64_t(0))return result;
    for(auto& m:s->maps)if(m.mapped==params->mappedResource)return result;
    for(auto& r:s->resources)if(r.registered==params->registeredResource&&r.format==params->mappedBufferFmt){for(auto& m:s->maps)if(!m.mapped){m={params->mappedResource,params->registeredResource,++s->nextIdentity,r.generation};return result;}}
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::unmapInput(void* encoder,NV_ENC_INPUT_PTR input){
    auto& self=*instance_.load(std::memory_order_acquire);const auto result=self.original_.nvEncUnmapInputResource(encoder,input);
    if(result==NV_ENC_SUCCESS){std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))for(auto& m:s->maps)if(m.mapped==input)m={};}return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::encode(void* encoder,NV_ENC_PIC_PARAMS* params){
    auto& self=*instance_.load(std::memory_order_acquire);const bool typed=params&&params->version==NV_ENC_PIC_PARAMS_VER;
    std::uint64_t identity=0,generation=0;
    if(typed&&params->inputBuffer&&params->outputBitstream&&!(params->encodePicFlags&NV_ENC_PIC_FLAG_EOS)){
        std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder);self.enabled_&&s&&s->initialized&&s->hevc&&s->nextIdentity!=~std::uint64_t(0)){
            generation=s->generation;
            for(auto& m:s->maps)if(m.mapped==params->inputBuffer)for(auto& r:s->resources)if(r.registered==m.registered&&r.generation==m.resourceGeneration&&r.width==params->inputWidth&&r.height==params->inputHeight&&r.width==s->width&&r.height==s->height&&r.format==params->bufferFmt){
                for(auto& p:s->submissions)if(!p.evidence.submission){identity=++s->nextIdentity;p.evidence={encoder,generation,identity,r.native,r.generation,m.generation,m.mapped,params->outputBitstream,params->inputTimeStamp,r.width,r.height,r.subresource,r.format,true};
                    if(self.inputObserver_.capture)try{self.inputObserver_.capture(p.evidence,self.inputObserver_.context);}catch(...){}
                    break;}
            }
        }
    }
    const auto result=self.original_.nvEncEncodePicture(encoder,params);
    if(identity){std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder);s&&s->generation==generation)for(auto& p:s->submissions)if(p.evidence.submission==identity){if(result==NV_ENC_SUCCESS||result==NV_ENC_ERR_NEED_MORE_INPUT){p.accepted=true;s->encoded=true;}else{if(self.inputObserver_.retire)try{self.inputObserver_.retire(p.evidence,self.inputObserver_.context);}catch(...){}p={};}}}
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::lock(void* encoder,NV_ENC_LOCK_BITSTREAM* params){
    auto& self=*instance_.load(std::memory_order_acquire);const bool typed=params&&params->version==NV_ENC_LOCK_BITSTREAM_VER;
    const auto result=self.original_.nvEncLockBitstream(encoder,params);
    if(result!=NV_ENC_SUCCESS||!typed||params->version!=NV_ENC_LOCK_BITSTREAM_VER)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);auto s=self.find(encoder);
    if(!self.enabled_||!s||s->output||!s->initialized||!s->hevc||
       !params->outputBitstream||!params->bitstreamBufferPtr||!params->bitstreamSizeInBytes||
       params->bitstreamSizeInBytes>MaximumAccessUnitBytes||params->sliceOffsets||params->alphaLayerSizeInBytes)return result;
    Session::Submission* matched=nullptr;bool ambiguous=false;
    for(auto& submission:s->submissions)if(submission.accepted&&!submission.locked&&submission.evidence.output==params->outputBitstream&&submission.evidence.inputTimestamp==params->outputTimeStamp){
        if(matched)ambiguous=true;
        matched=&submission;
    }
    NvencOutputEvidence evidence{};evidence.encoder=encoder;evidence.encoderGeneration=s->generation;evidence.output=params->outputBitstream;
    if(matched&&!ambiguous){
        matched->locked=true;evidence=matched->evidence;
        evidence.coverageSessionAuthorized=evidence.encoderGeneration==s->generation
            &&s->adapter.mutualConfirmed()&&s->adapter.probeObserved()
            &&(s->adapter.handshake().negotiatedCapabilities()&CapExplicitCoverage)!=0;
    }
    const Bytes original{static_cast<const std::uint8_t*>(params->bitstreamBufferPtr),params->bitstreamSizeInBytes};
    std::uint64_t ticket=0;
    try {
        Bytes candidate=original;
        if(s->adapter.requested()){
            auto prepared=s->adapter.prepareDiscovery(original,self.source(*s));
            if(!prepared.appended&&s->probeQueued&&s->adapter.mutualConfirmed())prepared=s->adapter.prepareProbe(original,self.source(*s),s->probeSequence,s->probeToken);
            if(prepared.appended){ticket=prepared.ticket;candidate=prepared.output;}
        }
        if(self.augmenter_.augment){
            s->augmented.clear();
            if(!self.augmenter_.augment(candidate,evidence,s->augmented,self.augmenter_.context)||s->augmented.empty()||s->augmented.size()>MaximumAccessUnitBytes){
                if(ticket)s->adapter.complete(ticket,false,s->channel);
                s->augmented.clear();return result;
            }
            candidate={s->augmented.data(),s->augmented.size()};
        }
        if(candidate.data!=original.data){s->output=params->outputBitstream;s->ticket=ticket;
            params->bitstreamBufferPtr=const_cast<std::uint8_t*>(candidate.data);
            params->bitstreamSizeInBytes=static_cast<std::uint32_t>(candidate.size);}
    }catch(...){if(ticket)s->adapter.complete(ticket,false,s->channel);s->augmented.clear();} // Exact original successful output on any refusal.

    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::unlock(void* encoder,NV_ENC_OUTPUT_PTR output){
    auto& self=*instance_.load(std::memory_order_acquire);const auto result=self.original_.nvEncUnlockBitstream(encoder,output);
    if(result!=NV_ENC_SUCCESS)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder);s){
        for(auto& submitted:s->submissions)if(submitted.accepted&&submitted.evidence.output==output){if(self.inputObserver_.retire)try{self.inputObserver_.retire(submitted.evidence,self.inputObserver_.context);}catch(...){}submitted={};}
        if(s->output!=output)return result;
        // Pinned driver copies every returned byte before this exact Unlock.
        if(s->ticket){s->adapter.complete(s->ticket,true,s->channel);if(s->adapter.probeObserved()||s->adapter.mutualConfirmed())s->probeQueued=false;}
        s->ticket=0;s->output=nullptr;s->augmented.clear();
    }
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::destroy(void* encoder){
    auto& self=*instance_.load(std::memory_order_acquire);
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))self.revoke(*s);}
    const auto result=self.original_.nvEncDestroyEncoder(encoder);
    if(result!=NV_ENC_SUCCESS)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder)){
        if(s->ticket)s->adapter.complete(s->ticket,false,s->channel);
        s->encoder=nullptr;s->generation=0;s->initialized=s->hevc=false;s->ticket=0;s->output=nullptr;s->augmented.clear();s->resources={};s->maps={};s->submissions={};
    }
    return result;
}
bool NvencBoundary::requestDiscovery(void* encoder,const LocalConfig& config,const VerifiedChannel& channel,PayloadSealer sealer){
    std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);
    if(!enabled_||!s||s->output||!s->initialized||!s->hevc)return false;
    const bool okay=s->adapter.request(config,channel,source(*s),sealer)==SenderStatus::WaitingPeer;
    if(okay)s->channel=channel;
    return okay;
}
Status NvencBoundary::receiveControl(void* encoder,Bytes payload,const VerifiedChannel& channel){
    std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);
    return s?s->adapter.receiveControl(payload,channel):Status::OutOfOrder;
}
bool NvencBoundary::requestProbe(void* encoder,std::uint64_t generation,std::uint64_t sequence,const Nonce& token){std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);if(!s||s->generation!=generation||!s->adapter.mutualConfirmed()||s->probeQueued||s->adapter.probeObserved())return false;s->probeSequence=sequence;s->probeToken=token;s->probeQueued=true;return true;}
Status NvencBoundary::receiveEcho(void* encoder,Bytes payload,const VerifiedChannel& channel){std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);return s?s->adapter.receiveEcho(payload,channel):Status::OutOfOrder;}
void NvencBoundary::setDeviceObserver(DeviceObserver value){std::lock_guard<std::mutex> guard(mutex_);deviceObserver_=value;}
void NvencBoundary::setInputObserver(InputObserver value){std::lock_guard<std::mutex> guard(mutex_);inputObserver_=value;}
void NvencBoundary::setOutputAugmenter(OutputAugmenter value){std::lock_guard<std::mutex> guard(mutex_);augmenter_=value;}
std::array<EncoderState,4> NvencBoundary::encoderStates()const{std::lock_guard<std::mutex> guard(mutex_);std::array<EncoderState,4> result{};for(unsigned i=0;i<sessions_.size();++i)result[i]={sessions_[i].encoder,sessions_[i].generation,sessions_[i].initialized&&sessions_[i].hevc,sessions_[i].adapter.mutualConfirmed(),sessions_[i].adapter.probeObserved(),sessions_[i].width,sessions_[i].height,sessions_[i].encoded};return result;}
void NvencBoundary::revokeDiscovery(void* encoder,std::uint64_t generation){std::lock_guard<std::mutex> guard(mutex_);if(auto s=find(encoder);s&&s->generation==generation)s->adapter.stop();}
void NvencBoundary::stop(){std::lock_guard<std::mutex> guard(mutex_);enabled_=false;for(auto& s:sessions_)revoke(s);}
bool NvencBoundary::wrapped()const{std::lock_guard<std::mutex> guard(mutex_);return installed_;}
std::uint64_t NvencBoundary::generation(void* e)const{std::lock_guard<std::mutex> guard(mutex_);for(const auto& s:sessions_)if(s.encoder==e&&e)return s.generation;return 0;}
std::uint32_t NvencBoundary::liveLeases()const{std::lock_guard<std::mutex> guard(mutex_);std::uint32_t count=0;for(const auto& s:sessions_)if(s.output)++count;return count;}
} // namespace stream_bridge::sender
