#include "pc/stream-bridge-sender-provider-core.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <exception>
#include <functional>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace {
unsigned checks=0,encodes=0,captures=0,retires=0,augments=0,event=0,captureEvent=0,encodeEvent=0;bool retirementValid=true;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void* encoder=reinterpret_cast<void*>(0x1010);void* resource=reinterpret_cast<void*>(0x2020);void* reg=reinterpret_cast<void*>(0x3030);void* mapped=reinterpret_cast<void*>(0x4040);void* output=reinterpret_cast<void*>(0x5050);
std::vector<std::uint8_t> au{0,0,0,1,0x26,1,0x80,0x34,0x56};
void* observedDevice=nullptr;std::uint64_t deviceGeneration=0;NV_ENC_DEVICE_TYPE observedDeviceType=NV_ENC_DEVICE_TYPE_CUDA;
NvencOutputEvidence captured{},observed{};bool allowAugment=false,wrongTimestamp=false;
std::uint64_t timestamp=0;NVENCSTATUS encodeResult=NV_ENC_SUCCESS,lockResult=NV_ENC_SUCCESS,unlockResult=NV_ENC_SUCCESS,reconfigureResult=NV_ENC_ERR_INVALID_PARAM;
NvencBoundary* activeBoundary=nullptr;PNVENCUNREGISTERRESOURCE nestedUnregister=nullptr;bool unregisterInside=false,discoveryRevokedInside=false;
unsigned localNonce=7;bool inspectDiscovery=true,inspectQos=false;
PNVENCENCODEPICTURE nestedEncode=nullptr;PNVENCLOCKBITSTREAM nestedLock=nullptr;NV_ENC_PIC_PARAMS* nestedPic=nullptr;
PNVENCUNLOCKBITSTREAM nestedUnlock=nullptr;bool inspectPendingQos=false;
NvencOutputEvidence pendingQosInput{};
std::function<void(void*)> duringEncode;
unsigned qosBarriers=0;bool reconfigureInside=false;PNVENCRECONFIGUREENCODER nestedReconfigure=nullptr;
LocalConfig localConfig(){LocalConfig c{};c.buildId=1;c.nonce[0]=static_cast<std::uint8_t>(localNonce);c.offered=c.required=KnownCapabilities;return c;}
VerifiedChannel authority(){VerifiedChannel a{};a.authenticated=true;a.binding[0]=8;return a;}
NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** e){*e=encoder;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI reconfigure(void* handle,NV_ENC_RECONFIGURE_PARAMS*){
    if(inspectDiscovery)discoveryRevokedInside=!activeBoundary->requestDiscovery(handle,localConfig(),authority());
    if(inspectPendingQos){
        // The actual output thread completes a previously accepted native
        // frame while the original same-contract Reconfigure is still running.
        // No sleep, speculative output or copied association logic is involved.
        std::exception_ptr failure;
        std::thread completion([&]{try{
            observed={};NV_ENC_LOCK_BITSTREAM value{};value.version=NV_ENC_LOCK_BITSTREAM_VER;value.outputBitstream=output;
            check(nestedLock(handle,&value)==NV_ENC_SUCCESS,"pending QoS preserves native output status");
            check(value.bitstreamBufferPtr!=au.data()&&observed.exactInput&&observed.coverageSessionAuthorized
                &&observed.submission==pendingQosInput.submission&&observed.inputTimestamp==pendingQosInput.inputTimestamp
                &&observed.resourceGeneration==pendingQosInput.resourceGeneration&&observed.mapGeneration==pendingQosInput.mapGeneration,
                "accepted frame completed DURING same-contract QoS retains its exact coverage receipt");
            check(nestedUnlock(handle,output)==NV_ENC_SUCCESS,"pending QoS completion releases only its real native output");
        }catch(...){failure=std::current_exception();}});
        completion.join();if(failure)std::rethrow_exception(failure);
    }
    if(inspectQos){
        const auto beforeCaptures=captures,beforeAugments=augments;
        check(nestedEncode(handle,nestedPic)==NV_ENC_SUCCESS,"native nested Encode still called during QoS");
        NV_ENC_LOCK_BITSTREAM value{};value.version=NV_ENC_LOCK_BITSTREAM_VER;value.outputBitstream=output;
        check(nestedLock(handle,&value)==NV_ENC_SUCCESS&&value.bitstreamBufferPtr!=au.data()
            &&captures==beforeCaptures+1&&augments==beforeAugments+1&&observed.exactInput&&observed.coverageSessionAuthorized,
            "native input accepted DURING same-contract QoS keeps its exact mask completion");
        check(nestedUnlock(handle,output)==NV_ENC_SUCCESS,"input completed during QoS retires its own exact native lease");++qosBarriers;
    }
    if(unregisterInside)nestedUnregister(handle,reg);
    if(reconfigureInside){reconfigureInside=false;NV_ENC_RECONFIGURE_PARAMS replacement{};replacement.version=NV_ENC_RECONFIGURE_PARAMS_VER;check(nestedReconfigure(handle,&replacement)==reconfigureResult,"nested actual source change forwarded during QoS");}
    return reconfigureResult;
}
NVENCSTATUS NVENCAPI registration(void*,NV_ENC_REGISTER_RESOURCE* p){p->registeredResource=reg;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unregistration(void*,NV_ENC_REGISTERED_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI map(void*,NV_ENC_MAP_INPUT_RESOURCE* p){p->mappedResource=mapped;p->mappedBufferFmt=NV_ENC_BUFFER_FORMAT_NV12;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unmap(void*,NV_ENC_INPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI encode(void* handle,NV_ENC_PIC_PARAMS* p){++encodes;encodeEvent=++event;timestamp=p->inputTimeStamp;if(duringEncode)duringEncode(handle);return encodeResult;}
NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM* p){p->bitstreamBufferPtr=au.data();p->bitstreamSizeInBytes=au.size();p->outputTimeStamp=timestamp+(wrongTimestamp?1:0);p->pictureType=NV_ENC_PIC_TYPE_IDR;return lockResult;}
NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR){return unlockResult;}
NVENCSTATUS NVENCAPI destroy(void*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* p){p->nvEncOpenEncodeSessionEx=open;p->nvEncInitializeEncoder=init;p->nvEncReconfigureEncoder=reconfigure;p->nvEncRegisterResource=registration;p->nvEncUnregisterResource=unregistration;p->nvEncMapInputResource=map;p->nvEncUnmapInputResource=unmap;p->nvEncEncodePicture=encode;p->nvEncLockBitstream=lock;p->nvEncUnlockBitstream=unlock;p->nvEncDestroyEncoder=destroy;return NV_ENC_SUCCESS;}
void device(void*,std::uint64_t generation,void* native,NV_ENC_DEVICE_TYPE type,void*){observedDevice=native;deviceGeneration=generation;observedDeviceType=type;}
void capture(const NvencOutputEvidence& e,void*){++captures;captureEvent=++event;captured=e;}
void retire(const NvencOutputEvidence& e,void*){retirementValid=retirementValid&&e.submission!=0;++retires;}
bool augment(Bytes candidate,const NvencOutputEvidence& e,std::vector<std::uint8_t>& owned,void*){++augments;observed=e;if(!allowAugment||!e.exactInput)return false;owned.assign(candidate.data,candidate.data+candidate.size);owned.push_back(0xab);return true;}
void run(){
    NvencBoundary bridge({{1}});activeBoundary=&bridge;bridge.setInputObserver({capture,retire,nullptr});bridge.setDeviceObserver({device,nullptr});bridge.setOutputAugmenter({augment,nullptr});
    NV_ENCODE_API_FUNCTION_LIST table{};table.version=NV_ENCODE_API_FUNCTION_LIST_VER;check(bridge.create(factory,&table)==NV_ENC_SUCCESS,"actual public input/output API table wrapped");
    void* handle=nullptr;NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};op.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;op.device=reinterpret_cast<void*>(0x9090);op.deviceType=NV_ENC_DEVICE_TYPE_DIRECTX;table.nvEncOpenEncodeSessionEx(&op,&handle);
    check(observedDevice==op.device&&observedDeviceType==op.deviceType&&deviceGeneration==bridge.generation(handle),"actual public Open device and epoch observed without guessed object layout");
    NV_ENC_CONFIG codecConfig{};codecConfig.version=NV_ENC_CONFIG_VER;codecConfig.rcParams.version=NV_ENC_CONFIG_VER;codecConfig.rcParams.averageBitRate=10000000;
    NV_ENC_INITIALIZE_PARAMS ip{};ip.encodeConfig=&codecConfig;ip.frameRateNum=90;ip.frameRateDen=1;ip.version=NV_ENC_INITIALIZE_PARAMS_VER;ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;ip.encodeWidth=1152;ip.encodeHeight=4608;table.nvEncInitializeEncoder(handle,&ip);
    check(!bridge.encoderStates()[0].encoded&&bridge.encoderStates()[0].width==1152&&bridge.encoderStates()[0].height==4608,"Init exposes actual dimensions but not successful encode authority");
    NV_ENC_REGISTER_RESOURCE rr{};rr.version=NV_ENC_REGISTER_RESOURCE_VER;rr.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;rr.bufferUsage=NV_ENC_INPUT_IMAGE;rr.resourceToRegister=resource;rr.width=1152;rr.height=4608;rr.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;
    check(table.nvEncRegisterResource(handle,&rr)==NV_ENC_SUCCESS&&rr.registeredResource==reg,"original registration result unchanged");
    NV_ENC_MAP_INPUT_RESOURCE mp{};mp.version=NV_ENC_MAP_INPUT_RESOURCE_VER;mp.registeredResource=reg;
    check(table.nvEncMapInputResource(handle,&mp)==NV_ENC_SUCCESS&&mp.mappedResource==mapped,"original actual mapped resource observed");
    NV_ENC_PIC_PARAMS pic{};pic.version=NV_ENC_PIC_PARAMS_VER;pic.inputBuffer=mapped;pic.outputBitstream=output;pic.inputWidth=1152;pic.inputHeight=4608;pic.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;pic.inputTimeStamp=0x1a2b3c4d55667788;
    auto get=[&](){NV_ENC_LOCK_BITSTREAM p{};p.version=NV_ENC_LOCK_BITSTREAM_VER;p.outputBitstream=output;check(table.nvEncLockBitstream(handle,&p)==NV_ENC_SUCCESS,"actual native output lock forwarded");return p;};
    auto release=[&](){check(table.nvEncUnlockBitstream(handle,output)==NV_ENC_SUCCESS,"actual native output unlock forwarded");};
    check(table.nvEncEncodePicture(handle,&pic)==NV_ENC_SUCCESS&&encodes==1&&captures==1&&captureEvent<encodeEvent,"one real Encode receives captured mapping");
    check(bridge.encoderStates()[0].encoded,"only accepted original Encode admits cold runtime bootstrap");
    auto p=get();check(!observed.coverageSessionAuthorized&&!observed.compressedCoverageAuthorized,"exact source input alone never authorizes a mask or compressed payload before negotiation");check(p.bitstreamBufferPtr==au.data()&&p.bitstreamSizeInBytes==au.size(),"configured augmenter refusal keeps original entire AU");
    check(observed.exactInput&&observed.resource==resource&&observed.encoder==encoder&&observed.mappedInput==mapped&&observed.output==output&&observed.inputTimestamp==pic.inputTimeStamp&&observed.width==1152&&observed.height==4608&&observed.subresource==0,"output exact public registry/map/encode timestamp tuple");
    check(observed.encoderGeneration==bridge.generation(handle)&&observed.resourceGeneration&&observed.mapGeneration>observed.resourceGeneration&&observed.submission>observed.mapGeneration&&observed.submission==captured.submission,"separate monotonically scoped resource/map/submission identities");release();check(retires==1,"input ledger retired at matching successful unlock");
    allowAugment=true;pic.inputTimeStamp++;table.nvEncEncodePicture(handle,&pic);p=get();check(p.bitstreamBufferPtr!=au.data()&&p.bitstreamSizeInBytes==au.size()+1&&bridge.liveLeases()==1,"owned final augmentation borrowed through actual native lease");
    const auto* borrowed=static_cast<const std::uint8_t*>(p.bitstreamBufferPtr);check(std::memcmp(borrowed,au.data(),au.size())==0&&borrowed[au.size()]==0xab,"candidate preserves original bytes, independent final storage");
    unlockResult=NV_ENC_ERR_GENERIC;check(table.nvEncUnlockBitstream(handle,output)==unlockResult&&bridge.liveLeases()==1&&borrowed[au.size()]==0xab,"failed Unlock preserves final certificate/mask bytes");unlockResult=NV_ENC_SUCCESS;release();check(bridge.liveLeases()==0&&retires==2,"successful Unlock retires final augmentation and source ledger");
    wrongTimestamp=true;pic.inputTimeStamp++;table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"unmatched timestamp never guesses most recent source");wrongTimestamp=false;release();
    encodeResult=NV_ENC_ERR_GENERIC;pic.inputTimeStamp++;table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"failed actual Encode never produces input evidence");release();encodeResult=NV_ENC_SUCCESS;
    pic.inputWidth=100;table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"extent mismatch preserves ordinary native output");release();pic.inputWidth=1152;
    table.nvEncUnmapInputResource(handle,mapped);table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"unmapped handle cannot regain resource identity");release();
    table.nvEncMapInputResource(handle,&mp);table.nvEncUnregisterResource(handle,reg);table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"unregistered input resource rejected");release();
    table.nvEncRegisterResource(handle,&rr);table.nvEncMapInputResource(handle,&mp);pic.inputTimeStamp++;table.nvEncEncodePicture(handle,&pic);table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"ambiguous reused output/timestamp not assigned to arbitrary submission");release();
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);p=get();const auto* held=static_cast<const std::uint8_t*>(p.bitstreamBufferPtr);
    check(held!=au.data()&&bridge.liveLeases()==1,"final augmentation lease exists independently of discovery ticket");
    NV_ENC_RECONFIGURE_PARAMS reconfig{};reconfig.version=NV_ENC_RECONFIGURE_PARAMS_VER;table.nvEncReconfigureEncoder(handle,&reconfig);
    check(bridge.liveLeases()==1&&held[au.size()]==0xab,"reconfiguration revokes inputs without freeing outstanding final bytes");release();
    // Native Reconfigure can keep registration/map handles alive. Reuse those
    // actual handles, but require fresh metadata, handshake and Encode evidence.
    table.nvEncInitializeEncoder(handle,&ip);table.nvEncRegisterResource(handle,&rr);table.nvEncMapInputResource(handle,&mp);
    check(bridge.requestDiscovery(handle,localConfig(),authority()),"pre-reconfigure explicit discovery exists");
    LocalConfig receiverConfig=localConfig();receiverConfig.role=Role::Receiver;receiverConfig.nonce[0]=90;Handshake peer;
    check(peer.begin(receiverConfig,authority())==Status::Ok,"independent compatible receiver handshake configured");
    auto sendFrame=[&](){++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);return get();};
    for(const auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){
        p=sendFrame();check(!observed.coverageSessionAuthorized,"handshake messages alone do not authorize coverage before probe");
        std::vector<std::uint8_t> body;check(inspectOwned({static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),p.bitstreamSizeInBytes},body)==SeiStatus::Ok,"actual coded AU carries exact discovery message");
        Message sent;check(decode({body.data(),body.size()},sent)==Status::Ok&&sent.kind==kind,"actual discovery order");release();
        check(peer.receive({body.data(),body.size()},authority())==Status::Ok,"independent peer consumes observed native output message");
        Message reply;check(peer.next(reply)==Status::Ok&&reply.kind==kind,"peer replies with matching next phase");
        std::array<std::uint8_t,MessageBytes> encoded{};check(encode(reply,encoded)==Status::Ok&&peer.markSent(reply,authority())==Status::Ok,"peer public handoff recorded");
        check(bridge.receiveControl(handle,{encoded.data(),encoded.size()},authority())==Status::Ok,"actual NVENC adapter consumes peer reply");
    }
    check(bridge.encoderStates()[0].peerConfirmed&&!bridge.encoderStates()[0].probeObserved,"mutual compatible session remains insufficient without coded-AU roundtrip");
    p=sendFrame();check(!observed.coverageSessionAuthorized,"mutual confirmation alone leaves mask authorization false");release();
    Nonce probeToken{};probeToken[0]=19;check(bridge.requestProbe(handle,bridge.generation(handle),1,probeToken),"fresh probe queued for current generation");
    p=sendFrame();check(!observed.coverageSessionAuthorized,"probe submission without actual echo leaves mask authorization false");
    std::vector<std::uint8_t> probeBytes;check(inspectOwned({static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),p.bitstreamSizeInBytes},probeBytes)==SeiStatus::Ok,"probe is observed in actual native coded AU");
    ProbeMessage probe,echo;check(decodeProbe({probeBytes.data(),probeBytes.size()},probe)==Status::Ok,"actual coded probe decoded");release();
    check(makeProbeEcho(peer,probe,authority(),echo)==Status::Ok,"peer echoes only actual observed probe");
    std::array<std::uint8_t,ProbeBytes> encodedEcho{};check(encodeProbe(echo,encodedEcho)==Status::Ok&&bridge.receiveEcho(handle,{encodedEcho.data(),encodedEcho.size()},authority())==Status::Ok,"matching current-generation roundtrip accepted");
    p=sendFrame();const auto before=observed;release();
    check(before.exactInput&&before.coverageSessionAuthorized&&before.compressedCoverageAuthorized,"actual Lock snapshots current-generation compatible mutual session plus observed probe and independent compressed-mask capability");
    const auto qosGeneration=bridge.generation(handle);const auto qosResources=before;
    NV_ENC_RECONFIGURE_PARAMS qos{};qos.version=NV_ENC_RECONFIGURE_PARAMS_VER;qos.reInitEncodeParams=ip;
    inspectDiscovery=false;
    nestedLock=table.nvEncLockBitstream;nestedUnlock=table.nvEncUnlockBitstream;
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);pendingQosInput=captured;
    inspectPendingQos=true;reconfigureResult=NV_ENC_SUCCESS;
    table.nvEncReconfigureEncoder(handle,&qos);inspectPendingQos=false;
    duringEncode=[&](void* nativeHandle){
        const auto expected=captured;std::exception_ptr failure;
        std::thread completion([&]{try{
            observed={};NV_ENC_LOCK_BITSTREAM value{};value.version=NV_ENC_LOCK_BITSTREAM_VER;value.outputBitstream=output;
            check(nestedLock(nativeHandle,&value)==NV_ENC_SUCCESS,"early native output completion preserves original status");
            check(observed.exactInput&&observed.coverageSessionAuthorized&&observed.submission==expected.submission
                &&observed.inputTimestamp==expected.inputTimestamp&&value.bitstreamBufferPtr!=au.data(),
                "successful exact native output BEFORE Encode returns keeps its current captured mask");
            check(nestedUnlock(nativeHandle,output)==NV_ENC_SUCCESS,"early completion unlock retires only its real captured input");
        }catch(...){failure=std::current_exception();}});
        completion.join();if(failure)std::rethrow_exception(failure);
    };
    ++pic.inputTimeStamp;const auto earlyRetirements=retires;
    check(table.nvEncEncodePicture(handle,&pic)==NV_ENC_SUCCESS,"native successful Encode status remains original after early output release");
    duringEncode={};
    check(retires==earlyRetirements+1&&bridge.liveLeases()==0,"completed-and-unlocked receipt is not resurrected when Encode finally returns");
    check(bridge.outputStats().completedBeforeEncodeReturn==1,"cold diagnostic records the exact early native completion without granting a different frame");
    duringEncode=[&](void* nativeHandle){
        const auto calls=augments;observed={};wrongTimestamp=true;
        NV_ENC_LOCK_BITSTREAM value{};value.version=NV_ENC_LOCK_BITSTREAM_VER;value.outputBitstream=output;
        check(nestedLock(nativeHandle,&value)==NV_ENC_SUCCESS&&value.bitstreamBufferPtr==au.data()
            &&augments==calls+1&&!observed.exactInput,
            "pending native Encode never makes a different successful output timestamp exact");
        wrongTimestamp=false;
    };
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);duringEncode={};
    p=get();check(observed.exactInput&&p.bitstreamBufferPtr!=au.data(),"only the later matching completed output can acquire the still-current input");release();
    duringEncode=[&](void* nativeHandle){
        const auto calls=augments;lockResult=NV_ENC_ERR_LOCK_BUSY;
        NV_ENC_LOCK_BITSTREAM value{};value.version=NV_ENC_LOCK_BITSTREAM_VER;value.outputBitstream=output;
        check(nestedLock(nativeHandle,&value)==NV_ENC_ERR_LOCK_BUSY&&augments==calls&&bridge.liveLeases()==0,
            "busy original native Lock cannot grant completion evidence while Encode is pending");
        lockResult=NV_ENC_SUCCESS;
    };
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);duringEncode={};
    p=get();check(observed.exactInput&&p.bitstreamBufferPtr!=au.data(),"successful retry matches only its current native receipt after a busy Lock");release();
    codecConfig.rcParams.averageBitRate=14000000;codecConfig.rcParams.maxBitRate=18000000;
    codecConfig.rcParams.vbvBufferSize=100000;codecConfig.rcParams.vbvInitialDelay=50000;qos.reInitEncodeParams.frameRateNum=120;
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);const auto asynchronousInput=captured;
    reconfigureResult=NV_ENC_SUCCESS;const auto unchanged=qos;const auto unchangedConfig=codecConfig;
    check(table.nvEncReconfigureEncoder(handle,&qos)==NV_ENC_SUCCESS,"actual same-contract bitrate/framerate adjustment forwarded");
    check(!std::memcmp(&qos,&unchanged,sizeof qos)&&!std::memcmp(&codecConfig,&unchangedConfig,sizeof codecConfig),"QoS never rewrites original init/config arguments");
    check(bridge.generation(handle)==qosGeneration&&bridge.encoderStates()[0].peerConfirmed&&bridge.encoderStates()[0].probeObserved,"QoS retains actual confirmed encoder session");
    for(unsigned i=0;i<2;++i)table.nvEncReconfigureEncoder(handle,&qos);
    p=get();check(observed.exactInput&&observed.coverageSessionAuthorized&&observed.submission==asynchronousInput.submission
        &&observed.inputTimestamp==asynchronousInput.inputTimestamp&&observed.resourceGeneration==asynchronousInput.resourceGeneration
        &&observed.mapGeneration==asynchronousInput.mapGeneration&&observed.encoderGeneration==qosGeneration,
        "Encode(A), repeated QoS, asynchronous Lock(A) keeps only that exact original accepted receipt");release();
    p=sendFrame();check(observed.exactInput&&observed.coverageSessionAuthorized&&observed.submission>qosResources.submission,"fresh Encode after QoS owns a new exact authorized submission");
    const auto* qosBorrowed=static_cast<const std::uint8_t*>(p.bitstreamBufferPtr);check(qosBorrowed!=au.data()&&bridge.liveLeases()==1,"actual output bytes borrowed before further QoS");
    codecConfig.rcParams.averageBitRate=17000000;reconfigureResult=NV_ENC_ERR_INVALID_PARAM;
    check(table.nvEncReconfigureEncoder(handle,&qos)==NV_ENC_ERR_INVALID_PARAM&&bridge.generation(handle)==qosGeneration,"rejected same-contract dynamic bitrate retains ongoing actual encoder");
    check(qosBorrowed[au.size()]==0xab&&bridge.liveLeases()==1,"accepted/rejected QoS never frees borrowed Lock bytes");release();
    nestedEncode=table.nvEncEncodePicture;nestedLock=table.nvEncLockBitstream;nestedPic=&pic;inspectQos=true;
    reconfigureResult=NV_ENC_SUCCESS;table.nvEncReconfigureEncoder(handle,&qos);inspectQos=false;check(qosBarriers==1,"actual original-call QoS barrier exercised");
    p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"already unlocked in-call input cannot be replayed after QoS returns");release();
    p=sendFrame();check(observed.exactInput&&observed.coverageSessionAuthorized,"fresh post-barrier Encode can resume exact coverage");release();
    qos.reInitEncodeParams.encodeConfig=nullptr;table.nvEncReconfigureEncoder(handle,&qos);
    check(bridge.generation(handle)==qosGeneration,"documented NULL config retains current deep-copied config with unchanged preset");
    qos.reInitEncodeParams.encodeConfig=&codecConfig;table.nvEncReconfigureEncoder(handle,&qos);
    check(bridge.generation(handle)==qosGeneration,"explicit identical config after NULL reconfigure remains proven");
    nestedUnregister=table.nvEncUnregisterResource;unregisterInside=true;table.nvEncReconfigureEncoder(handle,&qos);unregisterInside=false;
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"QoS never resurrects a resource unregistered by the native callback");release();
    table.nvEncRegisterResource(handle,&rr);table.nvEncMapInputResource(handle,&mp);
    p=sendFrame();check(observed.exactInput&&observed.coverageSessionAuthorized,"native re-registration gets fresh valid input after QoS");release();
    const auto qosStats=bridge.reconfigureStats();check(qosStats.preservedSuccessful==8&&qosStats.preservedRejected==1,"cold QoS counters distinguish successes from rejected attempts");
    check(qosStats.overlappingInputs==1&&qosStats.overlappingOutputs==2,"cold counters identify exact completions across both QoS overlap schedules");
    const auto paddingStart=offsetof(NV_ENC_INITIALIZE_PARAMS,privDataSize)+sizeof qos.reInitEncodeParams.privDataSize;
    const auto paddingEnd=offsetof(NV_ENC_INITIALIZE_PARAMS,privData);
    if(paddingEnd>paddingStart){
        auto* bytes=reinterpret_cast<unsigned char*>(&qos.reInitEncodeParams);bytes[paddingStart]=0xa5;
        table.nvEncReconfigureEncoder(handle,&qos);check(bridge.generation(handle)==qosGeneration,"caller alignment padding is not mistaken for changed video contract");bytes[paddingStart]=0;
    }
    inspectDiscovery=true;
    reconfig.reInitEncodeParams=ip;reconfig.resetEncoder=1;reconfigureResult=NV_ENC_SUCCESS;
    check(table.nvEncReconfigureEncoder(handle,&reconfig)==NV_ENC_SUCCESS&&discoveryRevokedInside,"native successful Reconfigure sees old authority revoked before return");
    const auto state=bridge.encoderStates()[0];
    check(state.hevc&&state.width==1152&&state.height==4608&&!state.encoded&&!state.peerConfirmed&&!state.probeObserved&&state.generation>before.encoderGeneration,"successful typed HEVC recovers source dimensions without stale peer/probe/encode proof");
    p=get();check(!observed.exactInput&&!observed.coverageSessionAuthorized&&p.bitstreamBufferPtr==au.data(),"old submission cannot authorize new generation before fresh Encode");release();
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);p=get();
    check(observed.exactInput&&!observed.coverageSessionAuthorized&&observed.encoderGeneration==state.generation&&observed.resourceGeneration>before.resourceGeneration&&observed.mapGeneration>before.mapGeneration&&observed.submission>before.submission,"retained native registrations/maps recover without re-registering but every metadata identity is fresh");release();
    check(bridge.receiveEcho(handle,{encodedEcho.data(),encodedEcho.size()},authority())!=Status::Ok,"old generation echo cannot restore image permission before cold runtime ticks");
    ++localNonce;check(bridge.requestDiscovery(handle,localConfig(),authority()),"fresh nonce negotiation can begin after actual native reconfigure");
    p=sendFrame();check(!observed.coverageSessionAuthorized,"fresh discovery does not inherit old session mask permission");release();
    reconfig.reInitEncodeParams.encodeWidth=2304;table.nvEncReconfigureEncoder(handle,&reconfig);
    check(bridge.encoderStates()[0].width==2304,"new actual coded dimensions published");
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"old resource dimensions cannot match new coded extent");release();
    reconfig.reInitEncodeParams=ip;table.nvEncReconfigureEncoder(handle,&reconfig);
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);p=get();check(observed.exactInput,"retained native input recovers when actual extent matches again without re-registration");release();
    nestedUnregister=table.nvEncUnregisterResource;unregisterInside=true;table.nvEncReconfigureEncoder(handle,&reconfig);unregisterInside=false;
    ++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);p=get();check(!observed.exactInput&&p.bitstreamBufferPtr==au.data(),"native unregister during original Reconfigure cannot be resurrected by a saved snapshot");release();
    table.nvEncRegisterResource(handle,&rr);table.nvEncMapInputResource(handle,&mp);
    ++reconfig.version;table.nvEncReconfigureEncoder(handle,&reconfig);--reconfig.version;
    check(!bridge.encoderStates()[0].hevc&&!bridge.requestDiscovery(handle,localConfig(),authority()),"successful unknown reconfigure ABI remains ineligible");
    table.nvEncInitializeEncoder(handle,&ip);table.nvEncRegisterResource(handle,&rr);table.nvEncMapInputResource(handle,&mp);
    // The retained snapshot must be independent of the caller's reused config.
    reconfig.resetEncoder=0;reconfig.reInitEncodeParams=ip;const auto deepGeneration=bridge.generation(handle);
    ++codecConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8;table.nvEncReconfigureEncoder(handle,&reconfig);
    check(bridge.generation(handle)>deepGeneration,"same caller pointer cannot hide a changed codec bit depth from the deep snapshot");
    --codecConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8;
    table.nvEncInitializeEncoder(handle,&ip);const auto reservedGeneration=bridge.generation(handle);
    reconfig.reInitEncodeParams.reserved[0]=1;table.nvEncReconfigureEncoder(handle,&reconfig);reconfig.reInitEncodeParams.reserved[0]=0;
    check(bridge.generation(handle)>reservedGeneration,"unknown reserved init data never qualifies for QoS preservation");
    table.nvEncInitializeEncoder(handle,&ip);const auto codecGeneration=bridge.generation(handle);
    codecConfig.reserved2[0]=reinterpret_cast<void*>(1);table.nvEncReconfigureEncoder(handle,&reconfig);codecConfig.reserved2[0]=nullptr;
    check(bridge.generation(handle)>codecGeneration,"unknown reserved config pointer never qualifies for QoS preservation");
    table.nvEncInitializeEncoder(handle,&ip);
    inspectDiscovery=false;nestedReconfigure=table.nvEncReconfigureEncoder;reconfigureInside=true;
    const auto nestedGeneration=bridge.generation(handle);table.nvEncReconfigureEncoder(handle,&reconfig);
    check(bridge.generation(handle)>nestedGeneration&&!bridge.encoderStates()[0].hevc,"outer successful QoS cannot resurrect nested native source invalidation");
    table.nvEncInitializeEncoder(handle,&ip);reconfig.reInitEncodeParams.encodeConfig=reinterpret_cast<NV_ENC_CONFIG*>(1);reconfigureResult=NV_ENC_ERR_INVALID_PARAM;
    check(table.nvEncReconfigureEncoder(handle,&reconfig)==NV_ENC_ERR_INVALID_PARAM&&!bridge.encoderStates()[0].hevc,"unreadable config is forwarded without early dereference or QoS permission");
    reconfig.reInitEncodeParams=ip;table.nvEncInitializeEncoder(handle,&ip);inspectDiscovery=true;
    reconfigureResult=NV_ENC_ERR_GENERIC;table.nvEncReconfigureEncoder(handle,&reconfig);
    check(!bridge.encoderStates()[0].hevc&&!bridge.requestDiscovery(handle,localConfig(),authority()),"failed known HEVC reconfigure remains ineligible");
    check(retirementValid,"all retirement callbacks carry an exact submission identity");
    table.nvEncDestroyEncoder(handle);bridge.stop();check(!bridge.encoderStates()[0].encoder,"destroyed source epoch not exposed to worker");
}
void olderCodecPeer(){
    NvencBoundary bridge({{1}});activeBoundary=&bridge;bridge.setOutputAugmenter({augment,nullptr});
    NV_ENCODE_API_FUNCTION_LIST table{};table.version=NV_ENCODE_API_FUNCTION_LIST_VER;
    check(bridge.create(factory,&table)==NV_ENC_SUCCESS,"older codec test uses actual wrapped NVENC boundary");
    void* handle=nullptr;NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};op.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    table.nvEncOpenEncodeSessionEx(&op,&handle);
    NV_ENC_INITIALIZE_PARAMS ip{};ip.version=NV_ENC_INITIALIZE_PARAMS_VER;ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;ip.encodeWidth=64;ip.encodeHeight=64;
    table.nvEncInitializeEncoder(handle,&ip);
    NV_ENC_REGISTER_RESOURCE rr{};rr.version=NV_ENC_REGISTER_RESOURCE_VER;rr.resourceToRegister=resource;rr.width=64;rr.height=64;
    rr.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;table.nvEncRegisterResource(handle,&rr);
    NV_ENC_MAP_INPUT_RESOURCE mp{};mp.version=NV_ENC_MAP_INPUT_RESOURCE_VER;mp.registeredResource=reg;table.nvEncMapInputResource(handle,&mp);
    NV_ENC_PIC_PARAMS pic{};pic.version=NV_ENC_PIC_PARAMS_VER;pic.inputBuffer=mapped;pic.outputBitstream=output;
    pic.inputWidth=pic.inputHeight=64;pic.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;
    auto local=localConfig();local.required&=~CapRleLz4;
    auto remote=local;remote.role=Role::Receiver;remote.nonce[0]=111;remote.offered&=~CapRleLz4;
    Handshake peer;check(peer.begin(remote,authority())==Status::Ok&&bridge.requestDiscovery(handle,local,authority()),"explicit optional-codec deployment starts older compatible peer");
    auto frame=[&](){++pic.inputTimeStamp;table.nvEncEncodePicture(handle,&pic);NV_ENC_LOCK_BITSTREAM value{};
        value.version=NV_ENC_LOCK_BITSTREAM_VER;value.outputBitstream=output;table.nvEncLockBitstream(handle,&value);return value;};
    for(const auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){
        auto value=frame();std::vector<std::uint8_t> body;
        check(!observed.compressedCoverageAuthorized&&inspectOwned({static_cast<const std::uint8_t*>(value.bitstreamBufferPtr),value.bitstreamSizeInBytes},body)==SeiStatus::Ok,"no compressed payload during optional legacy negotiation");
        table.nvEncUnlockBitstream(handle,output);
        check(peer.receive({body.data(),body.size()},authority())==Status::Ok,"older peer receives actual native AU control");
        Message reply;check(peer.next(reply)==Status::Ok&&reply.kind==kind,"older peer replies in order");
        std::array<std::uint8_t,MessageBytes> bytes{};check(encode(reply,bytes)==Status::Ok&&peer.markSent(reply,authority())==Status::Ok&&bridge.receiveControl(handle,{bytes.data(),bytes.size()},authority())==Status::Ok,"older peer signed channel reply consumed");
    }
    Nonce token{};token[0]=123;check(bridge.requestProbe(handle,bridge.generation(handle),1,token),"optional legacy session requests actual source probe");
    auto value=frame();std::vector<std::uint8_t> body;check(inspectOwned({static_cast<const std::uint8_t*>(value.bitstreamBufferPtr),value.bitstreamSizeInBytes},body)==SeiStatus::Ok,"legacy probe observed in actual AU");
    table.nvEncUnlockBitstream(handle,output);ProbeMessage probe,echo;
    check(decodeProbe({body.data(),body.size()},probe)==Status::Ok&&makeProbeEcho(peer,probe,authority(),echo)==Status::Ok,"legacy peer echoes exact observed probe");
    std::array<std::uint8_t,ProbeBytes> bytes{};check(encodeProbe(echo,bytes)==Status::Ok&&bridge.receiveEcho(handle,{bytes.data(),bytes.size()},authority())==Status::Ok,"legacy actual echo accepted");
    frame();check(observed.coverageSessionAuthorized&&!observed.compressedCoverageAuthorized,"actual exact Lock allows ordinary coverage but never unnegotiated LZ4 for an older peer");
    table.nvEncUnlockBitstream(handle,output);table.nvEncDestroyEncoder(handle);bridge.stop();
}
}
int main(){try{run();olderCodecPeer();std::printf("sender public input association: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"input association failed after %u: %s\n",checks,e.what());return 1;}}
