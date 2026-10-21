#include "stream-bridge-sender-provider-core.h"
#include <cstring>
#include <limits>
#ifdef __linux__
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace stream_bridge::sender {
namespace {
template<class T,std::size_t N> bool zero(const T (&values)[N]){for(const auto& value:values)if(value!=T{})return false;return true;}
void count(std::uint64_t& value){if(value!=std::numeric_limits<std::uint64_t>::max())++value;}
bool copyConfiguration(const NV_ENC_CONFIG* input,NV_ENC_CONFIG& output){
#ifdef _WIN32
    SIZE_T read=0;return ReadProcessMemory(GetCurrentProcess(),input,&output,sizeof output,&read)&&read==sizeof output;
#elif defined(__linux__)
    iovec local{&output,sizeof output},remote{const_cast<NV_ENC_CONFIG*>(input),sizeof output};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==static_cast<ssize_t>(sizeof output);
#else
    (void)input;(void)output;return false;
#endif
}
void padding(void* object,std::size_t begin,std::size_t end){if(end>begin)std::memset(static_cast<unsigned char*>(object)+begin,0,end-begin);}
}
static_assert(NVENCAPI_MAJOR_VERSION==11&&NVENCAPI_MINOR_VERSION==1,"SDK 11.1 only");
static_assert(sizeof(void*)==8&&sizeof(NV_ENCODE_API_FUNCTION_LIST)==0x9f8,"64-bit table ABI");
static_assert(NV_ENCODE_API_FUNCTION_LIST_VER==0x7102000b,"frozen table version");
static_assert(sizeof(NV_ENC_LOCK_BITSTREAM)==0x608&&NV_ENC_LOCK_BITSTREAM_VER==0x7101000b,"frozen lock ABI");
static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST,nvEncLockBitstream)==0x90&&
              offsetof(NV_ENCODE_API_FUNCTION_LIST,nvEncUnlockBitstream)==0x98,"source-proven copy/unlock slots");
std::atomic<NvencBoundary*> NvencBoundary::instance_{nullptr};
NvencBoundary::NvencBoundary(const std::array<std::uint8_t,32>& hash):hash_(hash){}
NvencBoundary::Session* NvencBoundary::find(void* e){for(auto& s:sessions_)if(s.encoder==e&&e)return &s;return nullptr;}
void NvencBoundary::retireSubmissions(Session& s){
    for(auto& p:s.submissions)if(p.evidence.submission&&inputObserver_.retire)try{inputObserver_.retire(p.evidence,inputObserver_.context);}catch(...){}
    s.submissions={};
}
std::uint64_t NvencBoundary::revoke(Session& s,bool retainRegistrations){
    s.adapter.stop();s.initialized=false;s.hevc=false;s.encoded=false;
    s.contractKnown=false;s.reconfiguring=false;s.wireBudget.reset();s.budgetApplied=false;retireSubmissions(s);
    if(!retainRegistrations){s.maps={};s.resources={};}
    s.probeQueued=false;
    s.generation=nextGeneration_==std::numeric_limits<std::uint64_t>::max()?0:++nextGeneration_;
    return s.generation;
}
bool NvencBoundary::snapshotContract(const NV_ENC_INITIALIZE_PARAMS& input,NV_ENC_INITIALIZE_PARAMS& init,NV_ENC_CONFIG& config,bool& hasConfig){
    if(input.version!=NV_ENC_INITIALIZE_PARAMS_VER||input.reservedBitFields||input.privDataSize||input.privData
        ||!zero(input.reserved)||!zero(input.reserved2))return false;
    for(const auto& hints:input.maxMEHintCountsPerBlock)if(hints.reserved||!zero(hints.reserved1))return false;
    // Copy fields into zeroed storage rather than treating caller padding as
    // part of a video contract. Reserved data still has to be explicitly zero.
    init={};init.version=input.version;init.encodeGUID=input.encodeGUID;init.presetGUID=input.presetGUID;
    init.encodeWidth=input.encodeWidth;init.encodeHeight=input.encodeHeight;init.darWidth=input.darWidth;init.darHeight=input.darHeight;
    init.frameRateNum=input.frameRateNum;init.frameRateDen=input.frameRateDen;init.enableEncodeAsync=input.enableEncodeAsync;init.enablePTD=input.enablePTD;
    init.reportSliceOffsets=input.reportSliceOffsets;init.enableSubFrameWrite=input.enableSubFrameWrite;init.enableExternalMEHints=input.enableExternalMEHints;
    init.enableMEOnlyMode=input.enableMEOnlyMode;init.enableWeightedPrediction=input.enableWeightedPrediction;init.enableOutputInVidmem=input.enableOutputInVidmem;
    init.maxEncodeWidth=input.maxEncodeWidth;init.maxEncodeHeight=input.maxEncodeHeight;
    std::memcpy(init.maxMEHintCountsPerBlock,input.maxMEHintCountsPerBlock,sizeof init.maxMEHintCountsPerBlock);
    init.tuningInfo=input.tuningInfo;init.bufferFormat=input.bufferFormat;
    hasConfig=input.encodeConfig!=nullptr;config={};
    if(!hasConfig)return true;
    // Snapshot the caller's pointed-to configuration BEFORE calling native code.
    // Its pointer, stack lifetime and later mutations are never stored as proof.
    if(!copyConfiguration(input.encodeConfig,config))return false;
    padding(&config,offsetof(NV_ENC_CONFIG,rcParams)+sizeof config.rcParams,offsetof(NV_ENC_CONFIG,encodeCodecConfig));
    padding(&config,offsetof(NV_ENC_CONFIG,reserved)+sizeof config.reserved,offsetof(NV_ENC_CONFIG,reserved2));
    auto& hevcConfig=config.encodeCodecConfig.hevcConfig;
    padding(&hevcConfig,offsetof(NV_ENC_CONFIG_HEVC,reserved1)+sizeof hevcConfig.reserved1,offsetof(NV_ENC_CONFIG_HEVC,reserved2));
    // The reviewed native caller uses CONFIG_VER in rcParams.version. Its
    // containing CONFIG_VER pins this layout; compare the inner value unchanged.
    const auto& rc=config.rcParams;const auto& hevc=config.encodeCodecConfig.hevcConfig;
    return config.version==NV_ENC_CONFIG_VER
        &&!rc.reservedBitField1&&!rc.reservedBitFields&&!rc.reserved2&&zero(rc.reserved1)&&zero(rc.reserved)
        &&zero(config.reserved)&&zero(config.reserved2)&&!hevc.reserved&&zero(hevc.reserved1)&&zero(hevc.reserved2)
        &&zero(hevc.hevcVUIParameters.reserved);
}
void NvencBoundary::saveContract(Session& s,const NV_ENC_INITIALIZE_PARAMS& init,const NV_ENC_CONFIG& config,bool hasConfig,bool known){
    s.contract=init;s.contractConfig=config;s.contractHasConfig=hasConfig;s.contractKnown=known;
}
bool NvencBoundary::sameContract(const Session& s,const NV_ENC_INITIALIZE_PARAMS& init,const NV_ENC_CONFIG& config,bool hasConfig){
    if(!s.contractKnown||!s.initialized||!s.hevc||s.reconfiguring)return false;
    auto previous=s.contract,candidate=init;
    // SDK11.1 Reconfigure explicitly retains the current configuration when
    // encodeConfig is NULL and the preset is unchanged. No preset is guessed.
    if(hasConfig&&!s.contractHasConfig)return false;
    previous.frameRateNum=previous.frameRateDen=candidate.frameRateNum=candidate.frameRateDen=0;
    if(std::memcmp(&previous,&candidate,sizeof previous))return false;
    if(!hasConfig)return true;
    auto oldConfig=s.contractConfig,newConfig=config;
    auto normalize=[](NV_ENC_RC_PARAMS& rc){rc.averageBitRate=rc.maxBitRate=rc.vbvBufferSize=rc.vbvInitialDelay=0;};
    normalize(oldConfig.rcParams);normalize(newConfig.rcParams);
    return std::memcmp(&oldConfig,&newConfig,sizeof oldConfig)==0;
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
    NV_ENC_INITIALIZE_PARAMS contract{};NV_ENC_CONFIG config{};bool hasConfig=false;
    const bool known=admissible&&hevc&&snapshotContract(*params,contract,config,hasConfig);
    std::uint64_t generation=0;
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))generation=self.revoke(*s);}
    const auto result=self.original_.nvEncInitializeEncoder(encoder,params);
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder);s&&generation&&s->generation==generation){
        s->initialized=result==NV_ENC_SUCCESS&&admissible;s->hevc=s->initialized&&hevc;s->width=width;s->height=height;
        saveContract(*s,contract,config,hasConfig,s->hevc&&known);
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
    NV_ENC_INITIALIZE_PARAMS contract{};NV_ENC_CONFIG config{};bool hasConfig=false;
    const bool known=hevc&&snapshotContract(*init,contract,config,hasConfig);
    std::uint64_t generation=0;bool qos=false;
    NV_ENC_RECONFIGURE_PARAMS budgeted{};NV_ENC_CONFIG budgetConfig{};bool applyBudget=false,applyingBudget=false;
    {
        std::lock_guard<std::mutex> guard(self.mutex_);count(self.reconfigureStats_.total);
        self.reconfigureStats_.lastWidth=width;self.reconfigureStats_.lastHeight=height;
        self.reconfigureStats_.lastContractKnown=known;self.reconfigureStats_.lastResetEncoder=typed&&params->resetEncoder;self.reconfigureStats_.lastSameContract=false;
        if(auto s=self.find(encoder)){
            const bool unchanged=known&&sameContract(*s,contract,config,hasConfig);self.reconfigureStats_.lastSameContract=unchanged;
            qos=self.enabled_&&unchanged&&!params->resetEncoder&&!params->reserved;
            if(qos){
                generation=s->generation;s->reconfiguring=true;
                const auto authority=s->adapter.mutualConfirmed()&&s->adapter.probeObserved()&&self.augmenter_.budgetAuthority?self.augmenter_.budgetAuthority(self.augmenter_.context):0;
                const auto metadata=s->wireBudget.reserve(authority,contract.frameRateNum,contract.frameRateDen);
                const auto& requestedConfig=hasConfig?config:s->contractConfig;
                const bool haveConfig=hasConfig||s->contractHasConfig;
                if(metadata&&haveConfig&&requestedConfig.rcParams.averageBitRate&&
                   (requestedConfig.rcParams.rateControlMode==NV_ENC_PARAMS_RC_CBR||requestedConfig.rcParams.rateControlMode==NV_ENC_PARAMS_RC_VBR
                    ||requestedConfig.rcParams.rateControlMode==NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ
                    ||requestedConfig.rcParams.rateControlMode==NV_ENC_PARAMS_RC_CBR_HQ||requestedConfig.rcParams.rateControlMode==NV_ENC_PARAMS_RC_VBR_HQ)){
                    budgeted=*params;budgetConfig=requestedConfig;
                    const auto requested=requestedConfig.rcParams.averageBitRate;
                    budgetConfig.rcParams.averageBitRate=WireBudget::videoRate(requested,metadata);
                    if(requestedConfig.rcParams.maxBitRate)budgetConfig.rcParams.maxBitRate=std::max(budgetConfig.rcParams.averageBitRate,WireBudget::videoRate(requestedConfig.rcParams.maxBitRate,metadata));
                    budgeted.reInitEncodeParams.encodeConfig=&budgetConfig;applyBudget=applyingBudget=true;
                    count(self.reconfigureStats_.budgeted);self.reconfigureStats_.requestedVideoRate=requested;
                    self.reconfigureStats_.appliedVideoRate=budgetConfig.rcParams.averageBitRate;self.reconfigureStats_.metadataRate=metadata;
                }else if(s->budgetApplied&&!hasConfig&&haveConfig){
                    // NULL retains the DRIVER's last rate, which may be our
                    // adjusted value. Restore the caller's saved original rate
                    // explicitly when the producer policy is withdrawn.
                    budgeted=*params;budgetConfig=requestedConfig;
                    budgeted.reInitEncodeParams.encodeConfig=&budgetConfig;applyBudget=true;
                }

            }else{generation=self.revoke(*s,hevc);count(self.reconfigureStats_.revoked);}
        }
    }
    // Only a fully compared bitrate/framerate change reaches this QoS path.
    // It does not alter input resources, codec, extent or the paired source.
    // Native Encode/Lock may overlap this original call on other threads. Keep
    // their normal exact receipts available DURING it as well as after return;
    // a temporary blanket barrier emitted an uncertified video frame. Changed,
    // resetting, nested or unknown contracts already revoke before this call.
    const auto result=self.original_.nvEncReconfigureEncoder(encoder,applyBudget?&budgeted:params);
    std::lock_guard<std::mutex> guard(self.mutex_);self.reconfigureStats_.lastStatus=static_cast<int>(result);
    if(auto s=self.find(encoder);s&&generation&&s->generation==generation){
        if(qos){
            s->reconfiguring=false;
            if(applyingBudget){if(result==NV_ENC_SUCCESS)count(self.reconfigureStats_.budgetAccepted);else count(self.reconfigureStats_.budgetRejected);}
            if(self.enabled_&&(result==NV_ENC_SUCCESS||result==NV_ENC_ERR_INVALID_PARAM)){
                if(result==NV_ENC_SUCCESS){
                    s->budgetApplied=applyingBudget;
                    // NULL means retain the existing deep-copied configuration.
                    if(!hasConfig){config=s->contractConfig;hasConfig=s->contractHasConfig;}
                    saveContract(*s,contract,config,hasConfig,true);count(self.reconfigureStats_.preservedSuccessful);
                }else count(self.reconfigureStats_.preservedRejected);
            }else{self.revoke(*s);count(self.reconfigureStats_.revoked);}
        }else if(self.enabled_&&result==NV_ENC_SUCCESS&&hevc&&s->nextIdentity<=std::numeric_limits<std::uint64_t>::max()-64){
            // Rebase CURRENT records, not a pre-call snapshot: a nested native
            // unregister/unmap must never be resurrected on return.
            for(auto& r:s->resources)if(r.registered)r.generation=++s->nextIdentity;
            for(auto& m:s->maps)if(m.mapped){
                Session::Resource* found=nullptr;for(auto& r:s->resources)if(r.registered==m.registered)found=&r;
                if(found){m.resourceGeneration=found->generation;m.generation=++s->nextIdentity;}else m={};
            }
            s->initialized=s->hevc=true;s->width=width;s->height=height;saveContract(*s,contract,config,hasConfig,known);
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
                    if(s->reconfiguring)count(self.reconfigureStats_.overlappingInputs);
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
    count(self.outputStats_.locks);
    Session::Submission* matched=nullptr;bool ambiguous=false;
    // The driver's completion thread may Lock/Unlock before the Encode caller
    // returns and publishes its CPU status. A SUCCESSFUL original Lock with the
    // unique live output/timestamp tuple is itself native completion evidence.
    // Keep the real captured resource/map/generation receipt; never choose a
    // latest input or admit a failed/busy original Lock. Failed Encode receipts
    // are removed on return, and real source changes revoke before native work.
    for(auto& submission:s->submissions)if(submission.evidence.submission&&!submission.locked&&submission.evidence.output==params->outputBitstream&&submission.evidence.inputTimestamp==params->outputTimeStamp){
        if(matched)ambiguous=true;
        matched=&submission;
    }
    NvencOutputEvidence evidence{};evidence.encoder=encoder;evidence.encoderGeneration=s->generation;evidence.output=params->outputBitstream;
    if(matched&&!ambiguous){
        count(self.outputStats_.exact);
        if(!matched->accepted)count(self.outputStats_.completedBeforeEncodeReturn);
        matched->accepted=matched->locked=true;s->encoded=true;evidence=matched->evidence;
        if(s->reconfiguring)count(self.reconfigureStats_.overlappingOutputs);
        evidence.coverageSessionAuthorized=evidence.encoderGeneration==s->generation
            &&s->adapter.mutualConfirmed()&&s->adapter.probeObserved()
            &&(s->adapter.handshake().negotiatedCapabilities()&CapExplicitCoverage)!=0;
        evidence.compressedCoverageAuthorized=evidence.coverageSessionAuthorized
            &&(s->adapter.handshake().negotiatedCapabilities()&CapRleLz4)!=0;
    }else count(self.outputStats_.unmatched);
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
            params->bitstreamSizeInBytes=static_cast<std::uint32_t>(candidate.size);
            const auto authority=evidence.coverageSessionAuthorized&&self.augmenter_.budgetAuthority?self.augmenter_.budgetAuthority(self.augmenter_.context):0;
            s->wireBudget.observe(authority,original.size,candidate.size);
        }
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
DiscoveryAdmission NvencBoundary::tryRequestDiscovery(void* encoder,const LocalConfig& config,const VerifiedChannel& channel,PayloadSealer sealer){
    std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);
    if(!enabled_||!s||!s->initialized||!s->hevc)return DiscoveryAdmission::Refused;
    // LockBitstream output is borrowed until native Unlock. Classify this under
    // the same mutex as discovery admission, avoiding a liveLeases() TOCTOU.
    if(s->output)return DiscoveryAdmission::Busy;
    const bool okay=s->adapter.request(config,channel,source(*s),sealer)==SenderStatus::WaitingPeer;
    if(okay){s->channel=channel;s->probeQueued=false;s->probeSequence=0;s->probeToken={};}
    return okay?DiscoveryAdmission::Ready:DiscoveryAdmission::Refused;
}
bool NvencBoundary::requestDiscovery(void* encoder,const LocalConfig& config,const VerifiedChannel& channel,PayloadSealer sealer){
    return tryRequestDiscovery(encoder,config,channel,sealer)==DiscoveryAdmission::Ready;
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
ReconfigureStats NvencBoundary::reconfigureStats()const{std::lock_guard<std::mutex> guard(mutex_);return reconfigureStats_;}
OutputStats NvencBoundary::outputStats()const{std::lock_guard<std::mutex> guard(mutex_);return outputStats_;}
std::array<EncoderState,4> NvencBoundary::encoderStates()const{std::lock_guard<std::mutex> guard(mutex_);std::array<EncoderState,4> result{};for(unsigned i=0;i<sessions_.size();++i)result[i]={sessions_[i].encoder,sessions_[i].generation,sessions_[i].initialized&&sessions_[i].hevc,sessions_[i].adapter.mutualConfirmed(),sessions_[i].adapter.probeObserved(),sessions_[i].width,sessions_[i].height,sessions_[i].encoded};return result;}
void NvencBoundary::revokeDiscovery(void* encoder,std::uint64_t generation){std::lock_guard<std::mutex> guard(mutex_);if(auto s=find(encoder);s&&s->generation==generation){s->adapter.stop();s->probeQueued=false;s->probeSequence=0;s->probeToken={};}}
void NvencBoundary::stop(){std::lock_guard<std::mutex> guard(mutex_);enabled_=false;for(auto& s:sessions_)revoke(s);}
bool NvencBoundary::wrapped()const{std::lock_guard<std::mutex> guard(mutex_);return installed_;}
std::uint64_t NvencBoundary::generation(void* e)const{std::lock_guard<std::mutex> guard(mutex_);for(const auto& s:sessions_)if(s.encoder==e&&e)return s.generation;return 0;}
std::uint32_t NvencBoundary::liveLeases()const{std::lock_guard<std::mutex> guard(mutex_);std::uint32_t count=0;for(const auto& s:sessions_)if(s.output)++count;return count;}
} // namespace stream_bridge::sender
