#include "pc/stream-bridge-sender-provider-core.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace {
unsigned checks=0,encodes=0,captures=0,retires=0,augments=0,event=0,captureEvent=0,encodeEvent=0;bool retirementValid=true;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void* encoder=reinterpret_cast<void*>(0x1010);void* resource=reinterpret_cast<void*>(0x2020);void* reg=reinterpret_cast<void*>(0x3030);void* mapped=reinterpret_cast<void*>(0x4040);void* output=reinterpret_cast<void*>(0x5050);
std::vector<std::uint8_t> au{0,0,0,1,0x26,1,0x80,0x34,0x56};
void* observedDevice=nullptr;std::uint64_t deviceGeneration=0;NV_ENC_DEVICE_TYPE observedDeviceType=NV_ENC_DEVICE_TYPE_CUDA;
NvencOutputEvidence captured{},observed{};bool allowAugment=false,wrongTimestamp=false;
std::uint64_t timestamp=0;NVENCSTATUS encodeResult=NV_ENC_SUCCESS,unlockResult=NV_ENC_SUCCESS;
NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** e){*e=encoder;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*){return NV_ENC_ERR_INVALID_PARAM;}
NVENCSTATUS NVENCAPI registration(void*,NV_ENC_REGISTER_RESOURCE* p){p->registeredResource=reg;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unregistration(void*,NV_ENC_REGISTERED_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI map(void*,NV_ENC_MAP_INPUT_RESOURCE* p){p->mappedResource=mapped;p->mappedBufferFmt=NV_ENC_BUFFER_FORMAT_NV12;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unmap(void*,NV_ENC_INPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI encode(void*,NV_ENC_PIC_PARAMS* p){++encodes;encodeEvent=++event;timestamp=p->inputTimeStamp;return encodeResult;}
NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM* p){p->bitstreamBufferPtr=au.data();p->bitstreamSizeInBytes=au.size();p->outputTimeStamp=timestamp+(wrongTimestamp?1:0);p->pictureType=NV_ENC_PIC_TYPE_IDR;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR){return unlockResult;}
NVENCSTATUS NVENCAPI destroy(void*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* p){p->nvEncOpenEncodeSessionEx=open;p->nvEncInitializeEncoder=init;p->nvEncReconfigureEncoder=reconfigure;p->nvEncRegisterResource=registration;p->nvEncUnregisterResource=unregistration;p->nvEncMapInputResource=map;p->nvEncUnmapInputResource=unmap;p->nvEncEncodePicture=encode;p->nvEncLockBitstream=lock;p->nvEncUnlockBitstream=unlock;p->nvEncDestroyEncoder=destroy;return NV_ENC_SUCCESS;}
void device(void*,std::uint64_t generation,void* native,NV_ENC_DEVICE_TYPE type,void*){observedDevice=native;deviceGeneration=generation;observedDeviceType=type;}
void capture(const NvencOutputEvidence& e,void*){++captures;captureEvent=++event;captured=e;}
void retire(const NvencOutputEvidence& e,void*){retirementValid=retirementValid&&e.submission!=0;++retires;}
bool augment(Bytes candidate,const NvencOutputEvidence& e,std::vector<std::uint8_t>& owned,void*){++augments;observed=e;if(!allowAugment||!e.exactInput)return false;owned.assign(candidate.data,candidate.data+candidate.size);owned.push_back(0xab);return true;}
void run(){
    NvencBoundary bridge({{1}});bridge.setInputObserver({capture,retire,nullptr});bridge.setDeviceObserver({device,nullptr});bridge.setOutputAugmenter({augment,nullptr});
    NV_ENCODE_API_FUNCTION_LIST table{};table.version=NV_ENCODE_API_FUNCTION_LIST_VER;check(bridge.create(factory,&table)==NV_ENC_SUCCESS,"actual public input/output API table wrapped");
    void* handle=nullptr;NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};op.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;op.device=reinterpret_cast<void*>(0x9090);op.deviceType=NV_ENC_DEVICE_TYPE_DIRECTX;table.nvEncOpenEncodeSessionEx(&op,&handle);
    check(observedDevice==op.device&&observedDeviceType==op.deviceType&&deviceGeneration==bridge.generation(handle),"actual public Open device and epoch observed without guessed object layout");
    NV_ENC_INITIALIZE_PARAMS ip{};ip.version=NV_ENC_INITIALIZE_PARAMS_VER;ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;ip.encodeWidth=1152;ip.encodeHeight=4608;table.nvEncInitializeEncoder(handle,&ip);
    NV_ENC_REGISTER_RESOURCE rr{};rr.version=NV_ENC_REGISTER_RESOURCE_VER;rr.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;rr.bufferUsage=NV_ENC_INPUT_IMAGE;rr.resourceToRegister=resource;rr.width=1152;rr.height=4608;rr.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;
    check(table.nvEncRegisterResource(handle,&rr)==NV_ENC_SUCCESS&&rr.registeredResource==reg,"original registration result unchanged");
    NV_ENC_MAP_INPUT_RESOURCE mp{};mp.version=NV_ENC_MAP_INPUT_RESOURCE_VER;mp.registeredResource=reg;
    check(table.nvEncMapInputResource(handle,&mp)==NV_ENC_SUCCESS&&mp.mappedResource==mapped,"original actual mapped resource observed");
    NV_ENC_PIC_PARAMS pic{};pic.version=NV_ENC_PIC_PARAMS_VER;pic.inputBuffer=mapped;pic.outputBitstream=output;pic.inputWidth=1152;pic.inputHeight=4608;pic.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;pic.inputTimeStamp=0x1a2b3c4d55667788;
    auto get=[&](){NV_ENC_LOCK_BITSTREAM p{};p.version=NV_ENC_LOCK_BITSTREAM_VER;p.outputBitstream=output;check(table.nvEncLockBitstream(handle,&p)==NV_ENC_SUCCESS,"actual native output lock forwarded");return p;};
    auto release=[&](){check(table.nvEncUnlockBitstream(handle,output)==NV_ENC_SUCCESS,"actual native output unlock forwarded");};
    check(table.nvEncEncodePicture(handle,&pic)==NV_ENC_SUCCESS&&encodes==1&&captures==1&&captureEvent<encodeEvent,"one real Encode receives captured mapping");
    auto p=get();check(p.bitstreamBufferPtr==au.data()&&p.bitstreamSizeInBytes==au.size(),"configured augmenter refusal keeps original entire AU");
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
    check(retirementValid,"all retirement callbacks carry an exact submission identity");
    table.nvEncDestroyEncoder(handle);bridge.stop();check(!bridge.encoderStates()[0].encoder,"destroyed source epoch not exposed to worker");
}
}
int main(){try{run();std::printf("sender public input association: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"input association failed after %u: %s\n",checks,e.what());return 1;}}
