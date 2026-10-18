#include "pc/stream-bridge-sender-provider-core.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace stream_bridge;
using namespace stream_bridge::sender;
namespace {
unsigned checks=0,factories=0,opens=0,locks=0,unlocks=0,destroys=0,inits=0,reconfigs=0;
NVENCSTATUS factoryStatus=NV_ENC_SUCCESS,lockStatus=NV_ENC_SUCCESS,unlockStatus=NV_ENC_SUCCESS,destroyStatus=NV_ENC_SUCCESS;
void* handle=reinterpret_cast<void*>(0x1010);void* output=reinterpret_cast<void*>(0x2020);
std::vector<std::uint8_t> originalAu{0,0,0,1,0x26,1,0x80,0x12,0x34};
NV_ENC_LOCK_BITSTREAM expected{};
void check(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
NVENCSTATUS NVENCAPI fakeOpen(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** encoder){++opens;if(encoder)*encoder=handle;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI fakeInit(void*,NV_ENC_INITIALIZE_PARAMS*){++inits;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI fakeReconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*){++reconfigs;return NV_ENC_ERR_INVALID_PARAM;}
NVENCSTATUS NVENCAPI fakeLock(void*,NV_ENC_LOCK_BITSTREAM* p){++locks;
 if(p&&lockStatus==NV_ENC_SUCCESS){p->bitstreamBufferPtr=originalAu.data();p->bitstreamSizeInBytes=static_cast<std::uint32_t>(originalAu.size());
  p->outputTimeStamp=0xaabbccdd12345678ull;p->outputDuration=12345;p->pictureType=NV_ENC_PIC_TYPE_IDR;p->frameAvgQP=17;p->frameIdx=23;expected=*p;}
 return lockStatus;}
NVENCSTATUS NVENCAPI fakeUnlock(void* encoder,NV_ENC_OUTPUT_PTR value){++unlocks;check(encoder==handle&&value==output,"original handles forwarded to unlock");return unlockStatus;}
NVENCSTATUS NVENCAPI fakeDestroy(void* encoder){++destroys;check(encoder==handle,"destroy original encoder");return destroyStatus;}
NVENCSTATUS NVENCAPI fakeFactory(NV_ENCODE_API_FUNCTION_LIST* t){++factories;if(factoryStatus!=NV_ENC_SUCCESS)return factoryStatus;
 if(t){t->nvEncOpenEncodeSessionEx=fakeOpen;t->nvEncInitializeEncoder=fakeInit;t->nvEncReconfigureEncoder=fakeReconfigure;
 t->nvEncLockBitstream=fakeLock;t->nvEncUnlockBitstream=fakeUnlock;t->nvEncDestroyEncoder=fakeDestroy;}return factoryStatus;}
Nonce nonce(unsigned n){Nonce v{};v[0]=static_cast<std::uint8_t>(n);return v;}
VerifiedChannel auth(){return {true,nonce(9)};}
LocalConfig config(unsigned n){LocalConfig c{};c.buildId=1;c.nonce=nonce(n);return c;}
NV_ENC_LOCK_BITSTREAM lockParams(){NV_ENC_LOCK_BITSTREAM p{};p.version=NV_ENC_LOCK_BITSTREAM_VER;p.outputBitstream=output;return p;}
void tests(){
 std::array<std::uint8_t,32> hash{};hash[0]=1;NvencBoundary bridge(hash);
 NV_ENCODE_API_FUNCTION_LIST bad{};bad.version=NV_ENCODE_API_FUNCTION_LIST_VER+1;
 check(bridge.create(fakeFactory,&bad)==NV_ENC_SUCCESS&&factories==1,"original factory called exactly once for unknown version");
 check(bad.nvEncLockBitstream==fakeLock&&!bridge.wrapped(),"unsupported table unchanged");
 factoryStatus=NV_ENC_ERR_OUT_OF_MEMORY;NV_ENCODE_API_FUNCTION_LIST fail{};fail.version=NV_ENCODE_API_FUNCTION_LIST_VER;auto copy=fail;
 check(bridge.create(fakeFactory,&fail)==factoryStatus&&factories==2&&std::memcmp(&fail,&copy,sizeof(fail))==0,"factory failure exactly preserved");
 factoryStatus=NV_ENC_SUCCESS;NV_ENCODE_API_FUNCTION_LIST table{};table.version=NV_ENCODE_API_FUNCTION_LIST_VER;
 check(bridge.create(fakeFactory,&table)==NV_ENC_SUCCESS&&factories==3&&bridge.wrapped(),"actual typed public factory table intercepted");
 check(table.nvEncLockBitstream!=fakeLock&&table.nvEncUnlockBitstream!=fakeUnlock,"public lock unlock callbacks replaced");
 void* encoder=nullptr;NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};open.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
 check(table.nvEncOpenEncodeSessionEx(&open,&encoder)==NV_ENC_SUCCESS&&encoder==handle&&opens==1,"original open once");
 auto generation=bridge.generation(handle);check(generation!=0,"actual open generates source epoch");
 auto p=lockParams();check(table.nvEncLockBitstream(encoder,&p)==NV_ENC_SUCCESS&&locks==1&&std::memcmp(&p,&expected,sizeof(p))==0,"inactive actual lock entire result byte identical");
 check(!bridge.requestDiscovery(encoder,config(1),auth()),"no actual init no source authority");
 NV_ENC_INITIALIZE_PARAMS init{};init.version=NV_ENC_INITIALIZE_PARAMS_VER;init.encodeGUID=NV_ENC_CODEC_H264_GUID;
 check(table.nvEncInitializeEncoder(encoder,&init)==NV_ENC_SUCCESS&&inits==1,"original init once");
 check(!bridge.requestDiscovery(encoder,config(1),auth()),"actual H264 not mislabeled HEVC");
 init.encodeGUID=NV_ENC_CODEC_HEVC_GUID;init.enableOutputInVidmem=1;table.nvEncInitializeEncoder(encoder,&init);
 check(!bridge.requestDiscovery(encoder,config(1),auth()),"video-memory output refused");
 init.enableOutputInVidmem=0;init.reportSliceOffsets=1;table.nvEncInitializeEncoder(encoder,&init);
 check(!bridge.requestDiscovery(encoder,config(1),auth()),"slice offsets not shifted by injected SEI");
 init.reportSliceOffsets=0;init.enableSubFrameWrite=1;table.nvEncInitializeEncoder(encoder,&init);
 check(!bridge.requestDiscovery(encoder,config(1),auth()),"subframe fragments never asserted complete AU");
 init.enableSubFrameWrite=0;table.nvEncInitializeEncoder(encoder,&init);
 auto unauth=auth();unauth.authenticated=false;check(!bridge.requestDiscovery(encoder,config(1),unauth),"no fabricated authenticated channel");
 check(bridge.requestDiscovery(encoder,config(1),auth()),"explicit test authority with actual public HEVC source");
 lockStatus=NV_ENC_ERR_LOCK_BUSY;p=lockParams();copy=table;auto originalParams=p;
 check(table.nvEncLockBitstream(encoder,&p)==lockStatus&&std::memcmp(&p,&originalParams,sizeof(p))==0,"lock errors and arguments unmodified");
 lockStatus=NV_ENC_SUCCESS;p=lockParams();++p.version;
 check(table.nvEncLockBitstream(encoder,&p)==NV_ENC_SUCCESS&&std::memcmp(&p,&expected,sizeof(p))==0,"unknown lock struct exact original result");
 p=lockParams();check(table.nvEncLockBitstream(encoder,&p)==NV_ENC_SUCCESS&&p.bitstreamBufferPtr!=originalAu.data(),"actual public output gets independent owned AU");
 const auto augmented=p;auto normalized=p;normalized.bitstreamBufferPtr=expected.bitstreamBufferPtr;normalized.bitstreamSizeInBytes=expected.bitstreamSizeInBytes;
 check(std::memcmp(&normalized,&expected,sizeof(p))==0,"only pointer and byte count changed, timestamps picture fields intact");
 std::vector<std::uint8_t> restored;check(removeOwned({static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),p.bitstreamSizeInBytes},restored)==SeiStatus::Ok&&restored==originalAu,"borrowed augmented AU contains exact original NAL bytes");
 auto saved=std::vector<std::uint8_t>(static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),static_cast<const std::uint8_t*>(p.bitstreamBufferPtr)+p.bitstreamSizeInBytes);
 check(bridge.liveLeases()==1,"lease registered to exact output handle");
 auto overlapping=lockParams();table.nvEncLockBitstream(encoder,&overlapping);check(std::memcmp(&overlapping,&expected,sizeof(p))==0,"overlapping lock remains original");
 unlockStatus=NV_ENC_ERR_GENERIC;check(table.nvEncUnlockBitstream(encoder,output)==unlockStatus&&bridge.liveLeases()==1,"failed unlock retains exact owned bytes");
 check(std::memcmp(saved.data(),augmented.bitstreamBufferPtr,saved.size())==0,"owned bytes valid until successful unlock");
 unlockStatus=NV_ENC_SUCCESS;check(table.nvEncUnlockBitstream(encoder,output)==NV_ENC_SUCCESS&&bridge.liveLeases()==0,"successful original unlock releases lease");
 check(bridge.requestDiscovery(encoder,config(2),auth()),"explicit new request");p=lockParams();table.nvEncLockBitstream(encoder,&p);check(bridge.liveLeases()==1,"second actual lease");
 saved.assign(static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),static_cast<const std::uint8_t*>(p.bitstreamBufferPtr)+p.bitstreamSizeInBytes);
 NV_ENC_RECONFIGURE_PARAMS reconfigure{};reconfigure.version=NV_ENC_RECONFIGURE_PARAMS_VER;
 check(table.nvEncReconfigureEncoder(encoder,&reconfigure)==NV_ENC_ERR_INVALID_PARAM&&reconfigs==1,"original reconfigure status");
 check(bridge.liveLeases()==1&&!bridge.requestDiscovery(encoder,config(3),auth()),"source change revokes without freeing borrowed output");
 check(std::memcmp(saved.data(),p.bitstreamBufferPtr,saved.size())==0,"revoked bytes preserved");table.nvEncUnlockBitstream(encoder,output);
 table.nvEncInitializeEncoder(encoder,&init);check(bridge.requestDiscovery(encoder,config(3),auth()),"explicit reinit establishes source");
 p=lockParams();table.nvEncLockBitstream(encoder,&p);destroyStatus=NV_ENC_ERR_GENERIC;
 check(table.nvEncDestroyEncoder(encoder)==destroyStatus&&bridge.liveLeases()==1,"failed destroy retains lease");
 destroyStatus=NV_ENC_SUCCESS;check(table.nvEncDestroyEncoder(encoder)==NV_ENC_SUCCESS&&bridge.liveLeases()==0&&bridge.generation(encoder)==0,"successful destroy retires source and lease");
 table.nvEncOpenEncodeSessionEx(&open,&encoder);check(bridge.generation(encoder)>generation,"reused handle gets fresh generation");table.nvEncInitializeEncoder(encoder,&init);
 check(bridge.requestDiscovery(encoder,config(4),auth()),"request new encoder epoch");p=lockParams();table.nvEncLockBitstream(encoder,&p);
 check(bridge.liveLeases()==1,"lease before Cleanup");bridge.stop();check(bridge.liveLeases()==1,"Cleanup retains borrowed data and callbacks");
 auto off=lockParams();table.nvEncLockBitstream(encoder,&off);check(std::memcmp(&off,&expected,sizeof(off))==0,"callbacks after Cleanup forward byte-identical output");
 table.nvEncUnlockBitstream(encoder,output);check(bridge.liveLeases()==0,"callback after Cleanup releases lease");
 table.nvEncDestroyEncoder(encoder);check(!bridge.requestDiscovery(encoder,config(5),auth()),"Cleanup never rearms automatically");
}
}
int main(){try{tests();std::printf("sender public NVENC boundary: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"provider failure after %u: %s\n",checks,e.what());return 1;}}
