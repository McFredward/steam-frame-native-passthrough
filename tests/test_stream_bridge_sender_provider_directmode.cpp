#include "pc/stream-bridge-sender-provider-directmode.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <limits>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0,creates=0,submits=0,presents=0,posts=0,seen=0;
void* object=reinterpret_cast<void*>(0x1234);DirectModeFunctions table{};DirectFrame frame{};bool destroyInPresent=false;
DirectMode::SwapTextureSetDesc_t forwarded{};const DirectMode::SwapTextureSetDesc_t* forwardedPointer=nullptr;
void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
void create(void*,std::uint32_t,const DirectMode::SwapTextureSetDesc_t* d,DirectMode::SwapTextureSet_t* o){++creates;forwardedPointer=d;if(d)forwarded=*d;for(unsigned i=0;i<3;++i)o->rSharedTextureHandles[i]=100*creates+i+1;}
void destroy(void*,vr::SharedTextureHandle_t){}
void destroyAll(void*,std::uint32_t){}
void next(void*,vr::SharedTextureHandle_t[2],std::uint32_t(*)[2]){}
void submit(void*,const DirectMode::SubmitLayerPerEye_t(&)[2]){++submits;}
void present(void* o,vr::SharedTextureHandle_t){++presents;if(const auto* p=DirectModeBoundary::currentFrame()){frame=*p;++seen;}if(destroyInPresent){table.destroyAll(o,42);check(!DirectModeBoundary::currentFrame(),"actual destruction invalidates in-flight snapshot epoch immediately");}}
void post(void*,const DirectMode::Throttling_t*){++posts;if(const auto* p=DirectModeBoundary::currentFrame()){check(p->presentSerial==frame.presentSerial,"PostPresent retains exact prior Present scope");++seen;}}
void timing(void*,vr::DriverDirectMode_FrameTiming*){}
void descriptors(){
    SharedTextureShape shape{};
    for(auto raw:{0u,1u,8u,9u}){
        check(decodePinnedSwapShape(4040,4040,27,raw,shape)&&shape.width==4040&&shape.height==4040&&shape.format==27&&shape.samples==1&&shape.arraySize==1,"pinned zero/default/packed single-sample descriptors normalize to actual native texture");
    }
    for(auto raw:{2u,4u,10u,12u})
        check(decodePinnedSwapShape(4040,4040,27,raw,shape)&&shape.samples==(raw&7u)&&shape.arraySize==1,"valid native MSAA metadata decoded without claiming single-sample coverage");
    for(auto raw:{3u,5u,6u,7u,11u,13u,14u,15u,16u,17u,24u,0xffffffffu}){
        shape={4040,4040,27,1,1};
        check(!decodePinnedSwapShape(4040,4040,27,raw,shape)&&!shape.width&&!shape.samples,"unsupported sample/array metadata fails closed and clears old shape");
    }
    check(decodePinnedSwapShape(16384,16384,27,9,shape),"maximum bounded extent accepted");
    for(const auto& d:{DirectMode::SwapTextureSetDesc_t{0,4040,27,9},{4040,0,27,9},{16385,4040,27,9},{4040,16385,27,9},{4040,4040,0,9}})
        check(!decodeSwapTextureSetDesc(d,shape),"empty/oversized extent and absent format refused");
}
void run(){
    descriptors();
    DirectModeBoundary bridge;check(!bridge.prepare({}),"incomplete original public table refused");check(bridge.prepare({create,destroy,destroyAll,next,submit,present,post,timing}),"typed DirectMode009 original table accepted");table=bridge.entryPoints();
    DirectMode::SwapTextureSetDesc_t desc{4040,4040,27,9};DirectMode::SwapTextureSet_t left{},right{};
    table.create(object,42,&desc,&left);table.create(object,42,&desc,&right);check(creates==2&&bridge.ownsProcess(42)&&!bridge.ownsProcess(43),"packed native descriptor 9 retains exact game ownership");
    check(forwardedPointer==&desc&&std::memcmp(&forwarded,&desc,sizeof desc)==0,"native CreateSwapTextureSet receives original pointer and unmodified packed descriptor");
    const auto inventory=bridge.inventory();check(inventory.valid&&inventory.epoch&&inventory.swaps[0].processId==42&&inventory.swaps[0].handles[0]==left.rSharedTextureHandles[0]&&inventory.swaps[1].generation!=inventory.swaps[0].generation,"cold source inventory retains actual owner and distinct generations");
    const auto revision=bridge.inventoryRevision();check(revision==inventory.epoch,"cheap cold revision agrees with the exact owned snapshot");
    DirectMode::SubmitLayerPerEye_t eyes[2]{};for(auto& eye:eyes){eye.bounds={0,0,1,1};eye.mProjection.m[0][0]=1;eye.mProjection.m[1][1]=1;eye.mHmdPose.m[0][0]=1;eye.mHmdPose.m[1][1]=1;eye.mHmdPose.m[2][2]=1;}
    eyes[0].hTexture=left.rSharedTextureHandles[1];eyes[1].hTexture=right.rSharedTextureHandles[2];
    check(!DirectModeBoundary::currentFrame(),"no claimed source outside actual Present/Post");table.submit(object,eyes);table.present(object,77);table.post(object,nullptr);
    check(seen==2&&submits==1&&presents==1&&posts==1&&frame.sourceKnown&&frame.layerCount==1,"actual forwarded two-eye layer visible only in native presentation calls");
    check(frame.component==object&&frame.layers[0][0].processId==42&&frame.layers[0][1].processId==42&&std::memcmp(&frame.layers[0][0].description,&desc,sizeof desc)==0&&std::memcmp(&frame.layers[0][1].description,&desc,sizeof desc)==0,"native component, PID and packed description exact in both submitted eyes");
    check(frame.layers[0][0].submitted.hTexture==eyes[0].hTexture&&frame.layers[0][1].submitted.hTexture==eyes[1].hTexture&&frame.layers[0][0].swapGeneration!=frame.layers[0][1].swapGeneration,"physical per-eye shared handles retain separate swap generations");
    check(std::memcmp(&frame.layers[0][0].submitted,&eyes[0],sizeof eyes[0])==0&&!DirectModeBoundary::currentFrame(),"bounds/projection/pose preserved byte-for-byte, TLS cleaned afterwards");
    auto count=seen;table.present(object,77);table.post(object,nullptr);check(seen==count,"no new layer cannot reuse an old source frame");
    auto saved=eyes[1].hTexture;eyes[1].hTexture=999;table.submit(object,eyes);table.present(object,77);check(seen==count,"unknown source handle never assigned another eye texture");eyes[1].hTexture=saved;
    eyes[0].bounds.uMin=std::numeric_limits<float>::quiet_NaN();table.submit(object,eyes);table.present(object,77);check(seen==count,"nonfinite native mapping not admitted");eyes[0].bounds.uMin=0;
    for(unsigned i=0;i<5;++i)table.submit(object,eyes);
    check(!bridge.inventoryRevision(),"tainted native collection cannot look like an unchanged admitted registry");
    table.present(object,77);check(seen==count,"overflowing native layer collection not partially certified");
    table.submit(object,eyes);table.present(object,77);check(seen==count+1&&bridge.inventoryRevision()==revision,"next bounded real frame recovers from incomplete collection without faking a registry mutation");
    destroyInPresent=true;table.submit(object,eyes);table.present(object,77);destroyInPresent=false;check(!bridge.ownsProcess(42),"DestroyAll removes current app ownership");check(bridge.inventoryRevision()!=revision,"real destruction invalidates the cheap cold revision immediately");count=seen;table.submit(object,eyes);table.present(object,77);check(seen==count,"destroyed shared handle not reusable as source identity");
    for(auto raw:{0u,4u,8u,9u}){auto d=desc;d.nSampleCount=raw;const auto pid=100u+raw;const auto calls=creates;table.create(object,pid,&d,&left);check(creates==calls+1&&bridge.ownsProcess(pid)&&std::memcmp(&forwarded,&d,sizeof d)==0,"supported packed/default descriptor retains owner with exact native pass-through");table.destroyAll(object,pid);}
    for(auto raw:{3u,7u,16u,17u,0xffffffffu}){auto d=desc;d.nSampleCount=raw;const auto calls=creates;table.create(object,500,&d,&left);check(creates==calls+1&&!bridge.ownsProcess(500)&&std::memcmp(&forwarded,&d,sizeof d)==0,"rejected coverage shape still forwards native creation untouched");}
    bridge.stop();check(!bridge.inventory().valid&&!bridge.inventoryRevision(),"stopped source registry cannot prepare texture imports or reuse a cold revision");table.submit(object,eyes);table.present(object,77);check(seen==count,"stop preserves original calls but removes all source authority");
}
}
int main(){try{run();std::printf("sender public DirectMode ownership: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"DirectMode failed after %u: %s\n",checks,e.what());return 1;}}
