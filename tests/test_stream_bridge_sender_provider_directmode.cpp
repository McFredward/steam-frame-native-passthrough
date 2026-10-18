#include "pc/stream-bridge-sender-provider-directmode.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <limits>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0,creates=0,submits=0,presents=0,posts=0,seen=0;
void* object=reinterpret_cast<void*>(0x1234);DirectModeFunctions table{};DirectFrame frame{};bool destroyInPresent=false;
void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
void create(void*,std::uint32_t,const DirectMode::SwapTextureSetDesc_t*,DirectMode::SwapTextureSet_t* o){++creates;for(unsigned i=0;i<3;++i)o->rSharedTextureHandles[i]=100*creates+i+1;}
void destroy(void*,vr::SharedTextureHandle_t){}
void destroyAll(void*,std::uint32_t){}
void next(void*,vr::SharedTextureHandle_t[2],std::uint32_t(*)[2]){}
void submit(void*,const DirectMode::SubmitLayerPerEye_t(&)[2]){++submits;}
void present(void* o,vr::SharedTextureHandle_t){++presents;if(const auto* p=DirectModeBoundary::currentFrame()){frame=*p;++seen;}if(destroyInPresent){table.destroyAll(o,42);check(!DirectModeBoundary::currentFrame(),"actual destruction invalidates in-flight snapshot epoch immediately");}}
void post(void*,const DirectMode::Throttling_t*){++posts;if(const auto* p=DirectModeBoundary::currentFrame()){check(p->presentSerial==frame.presentSerial,"PostPresent retains exact prior Present scope");++seen;}}
void timing(void*,vr::DriverDirectMode_FrameTiming*){}
void run(){
    DirectModeBoundary bridge;check(!bridge.prepare({}),"incomplete original public table refused");check(bridge.prepare({create,destroy,destroyAll,next,submit,present,post,timing}),"typed DirectMode009 original table accepted");table=bridge.entryPoints();
    DirectMode::SwapTextureSetDesc_t desc{4040,4040,28,1};DirectMode::SwapTextureSet_t left{},right{};
    table.create(object,42,&desc,&left);table.create(object,42,&desc,&right);check(creates==2&&bridge.ownsProcess(42)&&!bridge.ownsProcess(43),"actual CreateSwapTextureSet records owner PID without guessing");
    const auto inventory=bridge.inventory();check(inventory.valid&&inventory.epoch&&inventory.swaps[0].processId==42&&inventory.swaps[0].handles[0]==left.rSharedTextureHandles[0]&&inventory.swaps[1].generation!=inventory.swaps[0].generation,"cold source inventory retains actual owner and distinct generations");
    DirectMode::SubmitLayerPerEye_t eyes[2]{};for(auto& eye:eyes){eye.bounds={0,0,1,1};eye.mProjection.m[0][0]=1;eye.mProjection.m[1][1]=1;eye.mHmdPose.m[0][0]=1;eye.mHmdPose.m[1][1]=1;eye.mHmdPose.m[2][2]=1;}
    eyes[0].hTexture=left.rSharedTextureHandles[1];eyes[1].hTexture=right.rSharedTextureHandles[2];
    check(!DirectModeBoundary::currentFrame(),"no claimed source outside actual Present/Post");table.submit(object,eyes);table.present(object,77);table.post(object,nullptr);
    check(seen==2&&submits==1&&presents==1&&posts==1&&frame.sourceKnown&&frame.layerCount==1,"actual forwarded two-eye layer visible only in native presentation calls");
    check(frame.component==object&&frame.layers[0][0].processId==42&&frame.layers[0][1].processId==42&&frame.layers[0][0].description.nWidth==4040&&frame.layers[0][1].description.nHeight==4040,"native component, PID and description exact");
    check(frame.layers[0][0].submitted.hTexture==eyes[0].hTexture&&frame.layers[0][1].submitted.hTexture==eyes[1].hTexture&&frame.layers[0][0].swapGeneration!=frame.layers[0][1].swapGeneration,"physical per-eye shared handles retain separate swap generations");
    check(std::memcmp(&frame.layers[0][0].submitted,&eyes[0],sizeof eyes[0])==0&&!DirectModeBoundary::currentFrame(),"bounds/projection/pose preserved byte-for-byte, TLS cleaned afterwards");
    auto count=seen;table.present(object,77);table.post(object,nullptr);check(seen==count,"no new layer cannot reuse an old source frame");
    auto saved=eyes[1].hTexture;eyes[1].hTexture=999;table.submit(object,eyes);table.present(object,77);check(seen==count,"unknown source handle never assigned another eye texture");eyes[1].hTexture=saved;
    eyes[0].bounds.uMin=std::numeric_limits<float>::quiet_NaN();table.submit(object,eyes);table.present(object,77);check(seen==count,"nonfinite native mapping not admitted");eyes[0].bounds.uMin=0;
    for(unsigned i=0;i<5;++i)table.submit(object,eyes);
    table.present(object,77);check(seen==count,"overflowing native layer collection not partially certified");
    table.submit(object,eyes);table.present(object,77);check(seen==count+1,"next bounded real frame recovers from incomplete collection");
    destroyInPresent=true;table.submit(object,eyes);table.present(object,77);destroyInPresent=false;check(!bridge.ownsProcess(42),"DestroyAll removes current app ownership");count=seen;table.submit(object,eyes);table.present(object,77);check(seen==count,"destroyed shared handle not reusable as source identity");
    bridge.stop();check(!bridge.inventory().valid,"stopped source registry cannot prepare texture imports");table.submit(object,eyes);table.present(object,77);check(seen==count,"stop preserves original calls but removes all source authority");
}
}
int main(){try{run();std::printf("sender public DirectMode ownership: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"DirectMode failed after %u: %s\n",checks,e.what());return 1;}}
