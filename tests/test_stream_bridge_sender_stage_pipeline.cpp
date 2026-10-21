#include "pc/stream-bridge-sender-provider-core.h"
#include "pc/stream-bridge-sender-provider-coverage-stages.h"
#include "pc/stream-bridge-source-contract.h"
#include "bridge/stream_bridge_frame.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <functional>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace {
constexpr std::uint32_t Width=1152,Height=4608,Pid=123;
constexpr std::size_t Pixels=std::size_t(Width)*Height;
struct Resources{std::vector<std::uint8_t> pixels;};
using Stages=CoverageStages<Resources>;
Stages stages;SourceContract source;
unsigned checks=0,captures=0,masks=0,retirements=0;bool active=false;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::mutex barrierMutex;std::condition_variable barrier;
bool pauseRead=false,readerHeld=false,renderFinished=false;
std::uint64_t expectedPresent=0;unsigned metadataProtectedCopies=0;
NvencOutputEvidence lastCaptured{};
void* encoder=reinterpret_cast<void*>(0x1010);
const std::vector<std::uint8_t> au{0,0,0,1,0x26,1,0x80,0x34,0x56};
std::array<std::uint64_t,3> timestamps{};
std::uintptr_t index(void* p){return reinterpret_cast<std::uintptr_t>(p)-0x2000;}
void* input(unsigned i){return reinterpret_cast<void*>(std::uintptr_t(0x2000)+i);}
NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** e){*e=encoder;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*){return NV_ENC_SUCCESS;}
std::function<void()> duringQos;
std::function<void()> duringEncode;
NVENCSTATUS qosResult=NV_ENC_SUCCESS;
NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*){if(duringQos)duringQos();return qosResult;}
NVENCSTATUS NVENCAPI destroy(void*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI registration(void*,NV_ENC_REGISTER_RESOURCE* r){r->registeredResource=r->resourceToRegister;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unregistration(void*,NV_ENC_REGISTERED_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI map(void*,NV_ENC_MAP_INPUT_RESOURCE* p){p->mappedResource=p->registeredResource;p->mappedBufferFmt=NV_ENC_BUFFER_FORMAT_NV12;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unmap(void*,NV_ENC_INPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI encode(void*,NV_ENC_PIC_PARAMS* p){timestamps.at(index(p->outputBitstream))=p->inputTimeStamp;if(duringEncode)duringEncode();return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM* p){p->bitstreamBufferPtr=const_cast<std::uint8_t*>(au.data());p->bitstreamSizeInBytes=static_cast<std::uint32_t>(au.size());p->outputTimeStamp=timestamps.at(index(p->outputBitstream));p->pictureType=NV_ENC_PIC_TYPE_IDR;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* p){p->nvEncOpenEncodeSessionEx=open;p->nvEncInitializeEncoder=init;p->nvEncReconfigureEncoder=reconfigure;p->nvEncDestroyEncoder=destroy;p->nvEncRegisterResource=registration;p->nvEncUnregisterResource=unregistration;p->nvEncMapInputResource=map;p->nvEncUnmapInputResource=unmap;p->nvEncEncodePicture=encode;p->nvEncLockBitstream=lock;p->nvEncUnlockBitstream=unlock;return NV_ENC_SUCCESS;}
void capture(const NvencOutputEvidence& e,void*){
    if(!active)return;
    check(e.exactInput&&stages.bind(reinterpret_cast<std::uintptr_t>(e.resource),e.width,e.height,e.submission,e.encoder,e.encoderGeneration,100),"every exact native Encode binds while another stage/render metadata is owned");lastCaptured=e;++captures;
}
void retire(const NvencOutputEvidence& e,void*){
    auto owned=stages.read(e.submission,e.encoder,e.encoderGeneration);
    if(owned){owned->receipt={};owned.publish(Stages::Phase::Retired);++retirements;}
}
bool augment(Bytes candidate,const NvencOutputEvidence& e,std::vector<std::uint8_t>& output,void*){
    if(!active){output.assign(candidate.data,candidate.data+candidate.size);return true;}
    check(e.exactInput&&e.coverageSessionAuthorized,"real accepted native Lock has authenticated current-generation authority");
    auto owned=stages.read(e.submission,e.encoder,e.encoderGeneration);
    check(bool(owned),"every active native Lock owns its exact frozen stage");
    const auto& r=owned->receipt;
    check(r.present==expectedPresent&&r.width==Width&&r.height==Height&&source.current(r.contract,r.pid),"coverage uses exact source session/frame/extent, never latest or stale");
    if(pauseRead){
        std::unique_lock<std::mutex> held(barrierMutex);readerHeld=true;barrier.notify_all();
        barrier.wait(held,[]{return renderFinished;});
    }
    // This models CPU processing after a completed GPU query, at the full
    // production extent. The separate Windows fixture proves real GetData/Map
    // dispatch; this test proves accepted native source lifetime/concurrency.
    const auto pixels=owned->resources.pixels;
    check(pixels.size()==Pixels&&std::all_of(pixels.begin(),pixels.end(),[&](std::uint8_t p){return p==std::uint8_t(r.present);}),"all full-size pixels still belong to that accepted frame during concurrent copies");
    std::vector<std::uint8_t> encoded;CoverageEncoding encoding=CoverageEncoding::None;
    check(encodeBestCoverage({pixels.data(),pixels.size()},e.compressedCoverageAuthorized,encoding,encoded)&&encoding==CoverageEncoding::RleLz4,
        "actual negotiated native Lock compresses its full exact current mask");
    CertifiedAccessUnit certificate;certificate.descriptor={e.submission,e.encoderGeneration,Width,Height};certificate.coverageEncoding=encoding;certificate.coverage=encoded;
    std::vector<std::uint8_t> decoded;check(decodeCertifiedCoverage(certificate,decoded)&&decoded==pixels,"every compressed concurrent native submission retains all full-resolution coverage pixels");
    output.assign(candidate.data,candidate.data+candidate.size);output.insert(output.end(),encoded.begin(),encoded.end());++masks;return true;
}
void copy(unsigned i,std::uint64_t present){
    auto write=stages.capture(reinterpret_cast<std::uintptr_t>(input(i)));check(bool(write),"bounded native producer has an unowned exact stage");
    auto& r=write->receipt;r={};r.input=reinterpret_cast<std::uintptr_t>(input(i));r.contract=source.snapshot(Pid);r.present=present;r.pid=Pid;r.width=Width;r.height=Height;r.copyMicros=1;
    write->resources.pixels.assign(Pixels,static_cast<std::uint8_t>(present));write.publish(Stages::Phase::Captured);
}
LocalConfig config(){LocalConfig c{};c.buildId=1;c.nonce[0]=7;c.offered=c.required=KnownCapabilities;return c;}
VerifiedChannel authority(){VerifiedChannel a{};a.authenticated=true;a.binding[0]=8;return a;}
void run(){
    NvencBoundary boundary({{1}});boundary.setInputObserver({capture,retire,nullptr});boundary.setOutputAugmenter({augment,nullptr});
    NV_ENCODE_API_FUNCTION_LIST api{};api.version=NV_ENCODE_API_FUNCTION_LIST_VER;check(boundary.create(factory,&api)==NV_ENC_SUCCESS&&boundary.wrapped(),"production native ABI boundary installed");
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};op.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;void* handle=nullptr;api.nvEncOpenEncodeSessionEx(&op,&handle);
    NV_ENC_INITIALIZE_PARAMS ip{};ip.version=NV_ENC_INITIALIZE_PARAMS_VER;ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;ip.encodeWidth=Width;ip.encodeHeight=Height;ip.frameRateNum=90;ip.frameRateDen=1;api.nvEncInitializeEncoder(handle,&ip);
    for(unsigned i=0;i<3;++i){NV_ENC_REGISTER_RESOURCE rr{};rr.version=NV_ENC_REGISTER_RESOURCE_VER;rr.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;rr.bufferUsage=NV_ENC_INPUT_IMAGE;rr.resourceToRegister=input(i);rr.width=Width;rr.height=Height;rr.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;check(api.nvEncRegisterResource(handle,&rr)==NV_ENC_SUCCESS,"actual registered native input");NV_ENC_MAP_INPUT_RESOURCE mp{};mp.version=NV_ENC_MAP_INPUT_RESOURCE_VER;mp.registeredResource=rr.registeredResource;check(api.nvEncMapInputResource(handle,&mp)==NV_ENC_SUCCESS,"actual native mapped input");}
    std::uint64_t timestamp=1;
    auto submit=[&](unsigned i){NV_ENC_PIC_PARAMS p{};p.version=NV_ENC_PIC_PARAMS_VER;p.inputBuffer=input(i);p.outputBitstream=input(i);p.inputWidth=Width;p.inputHeight=Height;p.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;p.inputTimeStamp=timestamp++;check(api.nvEncEncodePicture(handle,&p)==NV_ENC_SUCCESS,"native Encode accepted");};
    auto get=[&](unsigned i){NV_ENC_LOCK_BITSTREAM p{};p.version=NV_ENC_LOCK_BITSTREAM_VER;p.outputBitstream=input(i);check(api.nvEncLockBitstream(handle,&p)==NV_ENC_SUCCESS,"native output Lock accepted");return p;};
    auto release=[&](unsigned i){check(api.nvEncUnlockBitstream(handle,input(i))==NV_ENC_SUCCESS,"matching native Unlock accepted");};
    submit(0);get(0);release(0);check(boundary.requestDiscovery(handle,config(),authority()),"native negotiated source bootstrap");
    auto peerConfig=config();peerConfig.role=Role::Receiver;peerConfig.nonce[0]=90;Handshake peer;check(peer.begin(peerConfig,authority())==Status::Ok,"independent receiver negotiation");
    for(auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){
        submit(0);auto p=get(0);std::vector<std::uint8_t> body;check(inspectOwned({static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),p.bitstreamSizeInBytes},body)==SeiStatus::Ok,"actual native AU carries negotiation");Message sent;check(decode({body.data(),body.size()},sent)==Status::Ok&&sent.kind==kind,"native discovery order");release(0);
        check(peer.receive({body.data(),body.size()},authority())==Status::Ok,"independent peer accepts native negotiation");Message reply;check(peer.next(reply)==Status::Ok,"peer response");std::array<std::uint8_t,MessageBytes> bytes{};check(encode(reply,bytes)==Status::Ok&&peer.markSent(reply,authority())==Status::Ok&&boundary.receiveControl(handle,{bytes.data(),bytes.size()},authority())==Status::Ok,"actual reply accepted");
    }
    Nonce token{};token[0]=19;check(boundary.requestProbe(handle,boundary.generation(handle),1,token),"real coded probe requested");submit(0);auto p=get(0);std::vector<std::uint8_t> body;check(inspectOwned({static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),p.bitstreamSizeInBytes},body)==SeiStatus::Ok,"real native probe AU");ProbeMessage probe,echo;check(decodeProbe({body.data(),body.size()},probe)==Status::Ok&&makeProbeEcho(peer,probe,authority(),echo)==Status::Ok,"observed probe echo");release(0);std::array<std::uint8_t,ProbeBytes> bytes{};check(encodeProbe(echo,bytes)==Status::Ok&&boundary.receiveEcho(handle,{bytes.data(),bytes.size()},authority())==Status::Ok,"current native generation roundtrip confirmed");
    check(source.publish(Pid),"current live app format granted");active=true;
    // Thirty deterministic three-frame batches cover 90 native frames. Pause
    // each first accepted Lock inside its stage read while the producer still
    // owns render metadata and publishes both following frames. No test sleeps
    // or timing races: the old shared try_lock would drop these source receipts.
    for(std::uint64_t first=1;first<=90;first+=3){
        copy(0,first);submit(0);expectedPresent=first;pauseRead=true;readerHeld=false;renderFinished=false;
        std::exception_ptr backgroundFailure;
        std::thread renderer([&]{try{
            {std::unique_lock<std::mutex> held(barrierMutex);barrier.wait(held,[]{return readerHeld;});}
            std::lock_guard<std::mutex> renderMetadata(barrierMutex);
            Stages::PoolClaim replacement;check(!stages.maintenance(replacement),"cold replacement cannot free a concurrently Reading stage");
            copy(1,first+1);copy(2,first+2);metadataProtectedCopies+=2;
            renderFinished=true;barrier.notify_all();
        }catch(...){backgroundFailure=std::current_exception();std::lock_guard<std::mutex> held(barrierMutex);renderFinished=true;barrier.notify_all();}});
        p=get(0);renderer.join();if(backgroundFailure)std::rethrow_exception(backgroundFailure);check(p.bitstreamBufferPtr!=au.data()&&p.bitstreamSizeInBytes>au.size(),"every active frame leaves native Lock with its current mask");pauseRead=false;release(0);
        for(unsigned i=1;i<3;++i){expectedPresent=first+i;submit(i);p=get(i);check(p.bitstreamBufferPtr!=au.data()&&p.bitstreamSizeInBytes>au.size(),"following source captured during read appears immediately with its own mask");release(i);}
    }
    check(captures==90&&masks==90&&retirements==90&&metadataProtectedCopies==60,"all 90 active accepted frames have masks; zero ordinary contention drops");
    // A foreign/stale native tuple cannot retire another accepted input, and a
    // partially claimed maintenance attempt preserves unrelated Captured data.
    copy(0,91);copy(1,92);submit(1);check(!stages.read(1,encoder,boundary.generation(handle)),"stale submission cannot borrow latest captured input");
    check(!stages.read(lastCaptured.submission,encoder,lastCaptured.encoderGeneration+1)&&!stages.read(lastCaptured.submission,input(0),lastCaptured.encoderGeneration),"foreign encoder/generation cannot retire another native frame");
    Stages::PoolClaim pending;check(!stages.maintenance(pending),"Bound stage excludes cold replacement after a partial Captured claim");expectedPresent=92;p=get(1);release(1);expectedPresent=91;submit(0);p=get(0);release(0);
    {check(stages.maintenance(pending),"all exact native stages retire independently");}
    pending={};
    // Complete real production native Lock and full-size mask encoding on an
    // output thread BEFORE the original QoS call returns. Alternate inputs
    // accepted before that call and inputs captured/accepted while it runs.
    // This is the formerly missing overlap in the after-return QoS checks.
    const auto qosGeneration=boundary.generation(handle);
    for(std::uint64_t present=93;present<=124;++present){
        const bool early=(present&1)!=0;
        if(early){copy(0,present);submit(0);}
        qosResult=(present&2)?NV_ENC_SUCCESS:NV_ENC_ERR_INVALID_PARAM;
        duringQos=[&]{
            if(!early){copy(0,present);submit(0);}
            expectedPresent=present;std::exception_ptr failure;
            std::thread completion([&]{try{
                const auto value=get(0);
                check(value.bitstreamBufferPtr!=au.data()&&value.bitstreamSizeInBytes>au.size(),
                    "every exact full-size frame completed DURING QoS keeps its mask");
                release(0);
            }catch(...){failure=std::current_exception();}});
            completion.join();if(failure)std::rethrow_exception(failure);
        };
        NV_ENC_RECONFIGURE_PARAMS qos{};qos.version=NV_ENC_RECONFIGURE_PARAMS_VER;qos.reInitEncodeParams=ip;
        qos.reInitEncodeParams.frameRateNum=120;
        check(api.nvEncReconfigureEncoder(handle,&qos)==qosResult&&boundary.generation(handle)==qosGeneration,
            "same-contract successful/rejected QoS preserves exact current session during native completion");
        duringQos={};
    }
    const auto continuity=boundary.reconfigureStats();
    check(captures==124&&masks==124&&retirements==124&&continuity.overlappingInputs==16&&continuity.overlappingOutputs==32,
        "all active frames have their own masks through both in-call QoS schedules without a blackout");
    for(std::uint64_t present=125;present<=140;++present){
        copy(0,present);expectedPresent=present;
        duringEncode=[&]{
            std::exception_ptr failure;
            std::thread completion([&]{try{
                const auto value=get(0);
                check(value.bitstreamBufferPtr!=au.data()&&value.bitstreamSizeInBytes>au.size(),
                    "every full-size exact native output completed BEFORE Encode returns keeps its mask");
                release(0);
            }catch(...){failure=std::current_exception();}});
            completion.join();if(failure)std::rethrow_exception(failure);
        };
        submit(0);duringEncode={};
    }
    check(captures==140&&masks==140&&retirements==140&&boundary.outputStats().completedBeforeEncodeReturn==16,
        "early native completion and later Encode return neither lose nor resurrect any exact frame");
    std::printf("PASS: %u checks; 140/140 active native frames fully masked at %ux%u; 60 concurrent native copies; 32 in-call QoS and 16 early Encode completions; no GPU wait or latest-mask fallback\n",checks,Width,Height);
}
}
int main(){try{run();return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
