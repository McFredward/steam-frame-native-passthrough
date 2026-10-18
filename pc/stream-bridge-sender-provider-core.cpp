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
std::uint64_t NvencBoundary::revoke(Session& s){
    s.adapter.stop();s.initialized=false;s.hevc=false;
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
       table->nvEncDestroyEncoder!=original_.nvEncDestroyEncoder||table->nvEncReconfigureEncoder!=original_.nvEncReconfigureEncoder))return result;
    if(!installed_){original_=*table;instance_.store(this,std::memory_order_release);installed_=true;}
    table->nvEncOpenEncodeSessionEx=open;table->nvEncInitializeEncoder=init;
    table->nvEncLockBitstream=lock;table->nvEncUnlockBitstream=unlock;
    table->nvEncDestroyEncoder=destroy;table->nvEncReconfigureEncoder=reconfigure;
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params,void** encoder){
    auto& self=*instance_.load(std::memory_order_acquire);const auto result=self.original_.nvEncOpenEncodeSessionEx(params,encoder);
    if(result!=NV_ENC_SUCCESS||!encoder||!*encoder)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);
    // Duplicate handles are not relabelled while a previous session still lives.
    if(!self.enabled_||self.find(*encoder)||self.nextGeneration_==std::numeric_limits<std::uint64_t>::max())return result;
    for(auto& s:self.sessions_)if(!s.encoder){s.encoder=*encoder;s.generation=++self.nextGeneration_;break;}
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::init(void* encoder,NV_ENC_INITIALIZE_PARAMS* params){
    auto& self=*instance_.load(std::memory_order_acquire);const bool admissible=params&&params->version==NV_ENC_INITIALIZE_PARAMS_VER;
    const bool hevc=admissible&&std::memcmp(&params->encodeGUID,&NV_ENC_CODEC_HEVC_GUID,sizeof(GUID))==0&&
        !params->enableOutputInVidmem&&!params->enableMEOnlyMode&&!params->reportSliceOffsets&&!params->enableSubFrameWrite;
    std::uint64_t generation=0;
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))generation=self.revoke(*s);}
    const auto result=self.original_.nvEncInitializeEncoder(encoder,params);
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder);s&&generation&&s->generation==generation){
        s->initialized=result==NV_ENC_SUCCESS&&admissible;s->hevc=s->initialized&&hevc;
    }
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::reconfigure(void* encoder,NV_ENC_RECONFIGURE_PARAMS* params){
    auto& self=*instance_.load(std::memory_order_acquire);
    // Never continue a negotiated source identity across any reconfiguration,
    // even a failed attempt. No borrowed AU is freed by revocation.
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder)){self.revoke(*s);}}
    return self.original_.nvEncReconfigureEncoder(encoder,params);
}
NVENCSTATUS NVENCAPI NvencBoundary::lock(void* encoder,NV_ENC_LOCK_BITSTREAM* params){
    auto& self=*instance_.load(std::memory_order_acquire);const bool typed=params&&params->version==NV_ENC_LOCK_BITSTREAM_VER;
    const auto result=self.original_.nvEncLockBitstream(encoder,params);
    if(result!=NV_ENC_SUCCESS||!typed||params->version!=NV_ENC_LOCK_BITSTREAM_VER)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);auto s=self.find(encoder);
    if(!self.enabled_||!s||!s->adapter.requested()||s->ticket||!s->initialized||!s->hevc||
       !params->outputBitstream||!params->bitstreamBufferPtr||!params->bitstreamSizeInBytes||
       params->bitstreamSizeInBytes>MaximumAccessUnitBytes||params->sliceOffsets||params->alphaLayerSizeInBytes)return result;
    try {
        const auto prepared=s->adapter.prepareDiscovery(
            {static_cast<const std::uint8_t*>(params->bitstreamBufferPtr),params->bitstreamSizeInBytes},self.source(*s));
        if(prepared.appended){s->output=params->outputBitstream;s->ticket=prepared.ticket;
            params->bitstreamBufferPtr=const_cast<std::uint8_t*>(prepared.output.data);
            params->bitstreamSizeInBytes=static_cast<std::uint32_t>(prepared.output.size);}
    }catch(...){} // Original successful output remains valid on allocation/parser refusal.
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::unlock(void* encoder,NV_ENC_OUTPUT_PTR output){
    auto& self=*instance_.load(std::memory_order_acquire);const auto result=self.original_.nvEncUnlockBitstream(encoder,output);
    if(result!=NV_ENC_SUCCESS)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder);s&&s->ticket&&s->output==output){
        // Pinned driver copies every returned byte before this exact Unlock.
        s->adapter.complete(s->ticket,true,s->channel);s->ticket=0;s->output=nullptr;
    }
    return result;
}
NVENCSTATUS NVENCAPI NvencBoundary::destroy(void* encoder){
    auto& self=*instance_.load(std::memory_order_acquire);
    {std::lock_guard<std::mutex> guard(self.mutex_);if(auto s=self.find(encoder))s->adapter.stop();}
    const auto result=self.original_.nvEncDestroyEncoder(encoder);
    if(result!=NV_ENC_SUCCESS)return result;
    std::lock_guard<std::mutex> guard(self.mutex_);
    if(auto s=self.find(encoder)){
        if(s->ticket)s->adapter.complete(s->ticket,false,s->channel);
        s->encoder=nullptr;s->generation=0;s->initialized=s->hevc=false;s->ticket=0;s->output=nullptr;
    }
    return result;
}
bool NvencBoundary::requestDiscovery(void* encoder,const LocalConfig& config,const VerifiedChannel& channel,PayloadSealer sealer){
    std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);
    if(!enabled_||!s||s->ticket||!s->initialized||!s->hevc)return false;
    const bool okay=s->adapter.request(config,channel,source(*s),sealer)==SenderStatus::WaitingPeer;
    if(okay)s->channel=channel;
    return okay;
}
Status NvencBoundary::receiveControl(void* encoder,Bytes payload,const VerifiedChannel& channel){
    std::lock_guard<std::mutex> guard(mutex_);auto s=find(encoder);
    return s?s->adapter.receiveControl(payload,channel):Status::OutOfOrder;
}
void NvencBoundary::stop(){std::lock_guard<std::mutex> guard(mutex_);enabled_=false;for(auto& s:sessions_)s.adapter.stop();}
bool NvencBoundary::wrapped()const{std::lock_guard<std::mutex> guard(mutex_);return installed_;}
std::uint64_t NvencBoundary::generation(void* e)const{std::lock_guard<std::mutex> guard(mutex_);for(const auto& s:sessions_)if(s.encoder==e&&e)return s.generation;return 0;}
std::uint32_t NvencBoundary::liveLeases()const{std::lock_guard<std::mutex> guard(mutex_);std::uint32_t count=0;for(const auto& s:sessions_)if(s.ticket)++count;return count;}
} // namespace stream_bridge::sender
