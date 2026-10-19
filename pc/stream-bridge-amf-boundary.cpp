#include "stream-bridge-amf-boundary.h"
#include <components/VideoEncoderHEVC.h>
#include <cerrno>
#include <limits>

namespace stream_bridge::sender {
namespace {
constexpr const wchar_t* Domain0=L"SteamLinkNativePassthrough.InputV1.Domain0";
constexpr const wchar_t* Domain1=L"SteamLinkNativePassthrough.InputV1.Domain1";
constexpr const wchar_t* Cookie=L"SteamLinkNativePassthrough.InputV1.Cookie";
bool integer(amf::AMFPropertyStorage* object,const wchar_t* key,std::int64_t& out) noexcept {
    amf::AMFVariant value;
    if(object->GetProperty(key,&value)!=AMF_OK||value.type!=amf::AMF_VARIANT_INT64)return false;
    out=value.int64Value;return true;
}
bool accepted(AMF_RESULT result) noexcept{return result==AMF_OK||result==AMF_NEED_MORE_INPUT;}
bool tag(amf::AMFData* input,const AmfContract& contract,std::uint64_t cookie) noexcept {
    // Never overwrite somebody else's namespaced metadata. The same input may
    // retain our old receipt after completed reuse; only our own domain may be
    // replaced, and no simultaneously pending use of that object is admitted.
    if(input->HasProperty(Domain0)||input->HasProperty(Domain1)||input->HasProperty(Cookie)){
        std::int64_t a=0,b=0,c=0;
        if(!integer(input,Domain0,a)||!integer(input,Domain1,b)||!integer(input,Cookie,c)
            ||a!=contract.propertyDomain[0]||b!=contract.propertyDomain[1]||c<=0)return false;
    }
    const auto set=[&](const wchar_t* name,std::int64_t value){return input->SetProperty(name,amf::AMFVariant(static_cast<amf_int64>(value)))==AMF_OK;};
    return set(Domain0,contract.propertyDomain[0])&&set(Domain1,contract.propertyDomain[1])&&set(Cookie,static_cast<std::int64_t>(cookie));
}
}
AmfBoundary::~AmfBoundary(){stop();if(component_)component_->Release();}
void AmfBoundary::retire(Entry& entry) noexcept {
    if(entry.earlyOutput)entry.earlyOutput->Release();
    if(entry.identity.inputObject)entry.identity.inputObject->Release();
    entry={};
}
void AmfBoundary::retireAll() noexcept{for(auto& entry:entries_)retire(entry);}
void AmfBoundary::publish(Entry& entry,amf::AMFBuffer* output) noexcept {
    auto identity=entry.identity;identity.output=output;
    if(observer_.output)observer_.output(identity,observer_.context);
    retire(entry);
}
bool AmfBoundary::prepare(amf::AMFComponent* component,AmfOriginals original,const AmfContract& contract,AmfObserver observer) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(component_||!component||!original.submit||!original.query||!original.flush||!original.reinit||!original.terminate)return false;
    // SDK frame-output property is inspected on the actual component. Merely
    // finding AMF or an AMD GPU is not a complete-AU/source proof.
    std::int64_t mode=-1;
    if(!contract.explicitlyEnabled||!contract.pinnedNativeFactory||!contract.hevcComponent||!contract.completeAnnexBFrames
        ||!contract.width||!contract.height||contract.width>16384||contract.height>16384
        ||!contract.propertyDomain[0]||!contract.propertyDomain[1]
        ||!integer(component,AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE,mode)||mode!=AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE_FRAME)return false;
    component->Acquire();component_=component;original_=original;contract_=contract;observer_=observer;
    generation_=1;enabled_=true;return true;
}
AMF_RESULT AmfBoundary::submit(amf::AMFData* input,const AmfInputIdentity& identity) noexcept {
    std::uint64_t cookie=0,generation=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!component_||!original_.submit)return AMF_NOT_INITIALIZED;
        bool overlapping=false;
        for(const auto& entry:entries_)if(input&&entry.identity.inputObject==input)overlapping=true;
        if(overlapping){
            // AMFData properties are not thread-safe/immutable. Once native
            // code resubmits an outstanding object, later property copying
            // could move a new cookie onto OLD pixels. A local retirement is
            // insufficient: keep the entire attachment unarmed thereafter.
            enabled_=false;retireAll();
        }
        if(enabled_&&input&&input->GetDataType()==amf::AMF_DATA_SURFACE&&identity.exactSourceAndCoverage
            &&identity.nativeResource&&identity.sourceGeneration&&identity.contentGeneration
            &&identity.width==contract_.width&&identity.height==contract_.height
            &&nextSubmission_<static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())){
            Entry* empty=nullptr;
            for(auto& entry:entries_){
                if(!entry.identity.submission&&!empty)empty=&entry;
            }
            if(empty){
                // Burn the identity even if public SetProperty fails; never
                // reuse a cookie left on a partially tagged native object.
                const auto candidate=++nextSubmission_;
                if(tag(input,contract_,candidate)){
                    cookie=candidate;generation=generation_;input->Acquire();
                    empty->identity={generation,cookie,identity,input,nullptr};
                }
            }
        }
    }
    const auto result=original_.submit(component_,input);const int nativeErrno=errno;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto& entry:entries_)if(cookie&&entry.identity.submission==cookie&&entry.identity.encoderGeneration==generation){
            if(accepted(result)){entry.accepted=true;if(entry.earlyOutput)publish(entry,entry.earlyOutput);}
            else retire(entry);
            break;
        }
    }
    errno=nativeErrno;return result;
}
AMF_RESULT AmfBoundary::query(amf::AMFData** output) noexcept {
    if(!component_||!original_.query)return AMF_NOT_INITIALIZED;
    const auto result=original_.query(component_,output);const int nativeErrno=errno;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(enabled_&&result==AMF_OK&&output&&*output&&(*output)->GetDataType()==amf::AMF_DATA_BUFFER){
            std::int64_t a=0,b=0,cookie=0;
            if(integer(*output,Domain0,a)&&integer(*output,Domain1,b)&&integer(*output,Cookie,cookie)
                &&a==contract_.propertyDomain[0]&&b==contract_.propertyDomain[1]&&cookie>0){
                for(auto& entry:entries_)if(entry.identity.submission==static_cast<std::uint64_t>(cookie)&&entry.identity.encoderGeneration==generation_){
                    amf::AMFBuffer* buffer=nullptr;
                    if((*output)->QueryInterface(amf::AMFBuffer::IID(),reinterpret_cast<void**>(&buffer))==AMF_OK&&buffer){
                        if(entry.accepted){publish(entry,buffer);buffer->Release();}
                        else if(!entry.earlyOutput)entry.earlyOutput=buffer;
                        else {buffer->Release();retire(entry);} // Replayed output is ambiguous.
                    }
                    break;
                }
            }
        }
    }
    errno=nativeErrno;return result;
}
AMF_RESULT AmfBoundary::flush() noexcept {
    if(!component_||!original_.flush)return AMF_NOT_INITIALIZED;
    const auto result=original_.flush(component_);const int nativeErrno=errno;
    if(result==AMF_OK){std::lock_guard<std::mutex> lock(mutex_);retireAll();if(generation_==~std::uint64_t(0))enabled_=false;else ++generation_;}
    errno=nativeErrno;return result;
}
AMF_RESULT AmfBoundary::reinit(amf_int32 width,amf_int32 height) noexcept {
    if(!component_||!original_.reinit)return AMF_NOT_INITIALIZED;
    const auto result=original_.reinit(component_,width,height);const int nativeErrno=errno;
    if(result==AMF_OK)stop(); // A fresh observed encoding/source contract is required.
    errno=nativeErrno;return result;
}
AMF_RESULT AmfBoundary::terminate() noexcept {
    if(!component_||!original_.terminate)return AMF_NOT_INITIALIZED;
    const auto result=original_.terminate(component_);const int nativeErrno=errno;
    if(result==AMF_OK)stop();
    errno=nativeErrno;return result;
}
void AmfBoundary::stop() noexcept{std::lock_guard<std::mutex> lock(mutex_);enabled_=false;retireAll();}
std::size_t AmfBoundary::pending()const noexcept{std::lock_guard<std::mutex> lock(mutex_);std::size_t count=0;for(const auto& entry:entries_)if(entry.identity.submission)++count;return count;}
bool AmfBoundary::active()const noexcept{std::lock_guard<std::mutex> lock(mutex_);return enabled_;}
} // namespace stream_bridge::sender
