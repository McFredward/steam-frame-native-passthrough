#include "pc/stream-bridge-sender-provider-coverage.h"
#include <d3d11_4.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
using namespace stream_bridge;
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->()const{return p;}};
NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** value){*value=reinterpret_cast<void*>(0x1010);return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM*){return NV_ENC_ERR_LOCK_BUSY;}
NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI destroy(void*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* table){
    table->nvEncOpenEncodeSessionEx=open;table->nvEncInitializeEncoder=init;
    table->nvEncReconfigureEncoder=reconfigure;table->nvEncLockBitstream=lock;
    table->nvEncUnlockBitstream=unlock;table->nvEncDestroyEncoder=destroy;
    return NV_ENC_SUCCESS;
}
// This is deliberately a real native context, not a fake COM object. The
// public CreateDevice output is retained on the rendering thread while the
// production observer obtains GetImmediateContext and installs on its worker.
void run(){
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> submittedContext;D3D_FEATURE_LEVEL level{};
    const auto result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,submittedContext.out());
    check(SUCCEEDED(result)&&level>=D3D_FEATURE_LEVEL_11_0,"actual native D3D11 WARP context created");
    Com<ID3D11Multithread> threading;
    check(SUCCEEDED(submittedContext->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(threading.out()))),"native threading interface");
    threading->SetMultithreadProtected(TRUE); // The pinned native caller does this before NVENC Open.
    void** const before=*reinterpret_cast<void***>(submittedContext.p);
    const auto drawBefore=before[13],mapBefore=before[14],unmapBefore=before[15],copyBefore=before[47];
    NvencBoundary boundary({{1}});DirectModeBoundary direct;ControlRuntime control;
    SenderCoverage coverage(direct,boundary,control);
    check(coverage.configure(1),"production observer configured without granting a native callsite");
    NV_ENCODE_API_FUNCTION_LIST api{};api.version=NV_ENCODE_API_FUNCTION_LIST_VER;
    check(boundary.create(factory,&api)==NV_ENC_SUCCESS,"production public NVENC Open boundary installed");
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS parameters{};parameters.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    parameters.device=device.p;parameters.deviceType=NV_ENC_DEVICE_TYPE_DIRECTX;void* encoder=nullptr;
    check(api.nvEncOpenEncodeSessionEx(&parameters,&encoder)==NV_ENC_SUCCESS,"production observer receives actual native D3D11 device");
    std::thread worker([&]{coverage.tick();});worker.join();
    check(coverage.gateStats().deviceOpened==1&&coverage.gateStats().contextsReady==1,"actual production context hook installation completed on worker");
    auto** const installed=*reinterpret_cast<void***>(submittedContext.p);
    check(installed[13]!=drawBefore&&installed[14]!=mapBefore&&installed[15]!=unmapBefore&&installed[47]!=copyBefore,"all four actual CreateDevice-context dispatch slots intercepted");
    submittedContext->Draw(0,0);
    auto stats=coverage.gateStats();
    check(stats.hookDraws==1&&stats.otherDraws==1&&stats.nativeDraws==0,"CreateDevice output Draw reaches production hook on rendering thread");
    check(stats.sourceFrames==0&&stats.certifiedMasks==0,"fixture call cannot authorize a fabricated native source");
    Com<ID3D11DeviceContext> queried;std::thread queryWorker([&]{device->GetImmediateContext(queried.out());});queryWorker.join();
    queried->Draw(0,0);
    check(coverage.gateStats().hookDraws==2,"cold GetImmediateContext alias reaches the same production observer");
    Com<ID3D11DeviceContext1> extended;
    check(SUCCEEDED(submittedContext->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(extended.out()))),"native versioned context interface");
    extended->Draw(0,0);
    check(coverage.gateStats().hookDraws==3,"versioned interface dispatch reaches production hook");
    submittedContext->ClearState();threading->SetMultithreadProtected(FALSE);threading->SetMultithreadProtected(TRUE);
    submittedContext->Draw(0,0);
    check(coverage.gateStats().hookDraws==4,"native ClearState and protection changes retain dispatch hooks");
    // Native CopyResource/Map/Unmap still execute through the real installed
    // table and preserve bytes; no fake return-address proof is introduced.
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;
    const std::array<unsigned char,64> bytes{{17,29,43,101}};D3D11_SUBRESOURCE_DATA initial{bytes.data(),16,0};
    Com<ID3D11Texture2D> source,staging;
    check(SUCCEEDED(device->CreateTexture2D(&desc,&initial,source.out())),"actual native source resource");
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,staging.out())),"actual native staging resource");
    submittedContext->CopyResource(staging.p,source.p);D3D11_MAPPED_SUBRESOURCE mapped{};
    check(SUCCEEDED(submittedContext->Map(staging.p,0,D3D11_MAP_READ,0,&mapped)),"actual intercepted native Map succeeds");
    check(mapped.pData&&std::memcmp(mapped.pData,bytes.data(),16)==0,"ordinary native RGB bytes preserved through production hooks");
    submittedContext->Unmap(staging.p,0);
    coverage.stop();
    auto** const restored=*reinterpret_cast<void***>(submittedContext.p);
    check(restored[13]==drawBefore&&restored[14]==mapBefore&&restored[15]==unmapBefore&&restored[47]==copyBefore,"production stop restores only the original owned dispatch slots");
}
}
int main(){try{run();std::printf("production native D3D11 context observer: %u checks PASS\n",checks);return 0;}
catch(const std::exception& e){std::fprintf(stderr,"native context observer failed after %u: %s\n",checks,e.what());return 1;}}
