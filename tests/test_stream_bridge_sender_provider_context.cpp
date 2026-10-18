#include "pc/stream-bridge-sender-provider-coverage.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
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
using NativeUnmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
NativeUnmap otherPreviousUnmap=nullptr;
void STDMETHODCALLTYPE otherUnmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub){otherPreviousUnmap(context,resource,sub);}
template<class To,class From>To pointerBits(From value){static_assert(sizeof(To)==sizeof(From),"same Win64 pointer size");To out{};std::memcpy(&out,&value,sizeof out);return out;}
void replace(void** slot,void* before,void* after){
    DWORD previous=0;check(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&previous)!=0,"fixture owns one temporary external hook slot write");
    void* observed=InterlockedCompareExchangePointer(slot,after,before);DWORD ignored=0;
    check(VirtualProtect(slot,sizeof(void*),previous,&ignored)!=0&&observed==before,"fixture external hook replacement is exact and restored protection");
}
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
void diagnostic(const char* phase,const CoverageGateStats& stats,bool sameCreated){
    std::printf("context diagnostic %s: draws=%llu, other=%llu, same-created=%u, readable=%u, stable=%u, native-device-same=%u, slots=%u, originals=%u\n",
        phase,static_cast<unsigned long long>(stats.hookDraws),static_cast<unsigned long long>(stats.otherDraws),unsigned(sameCreated),
        unsigned(stats.contextTableReadable),unsigned(stats.contextTableStable),unsigned(stats.nativeDeviceSame),stats.contextHookSlots,stats.contextOriginalSlots);
    std::fflush(stdout);
}
void pipeline(ID3D11Device* device,ID3D11DeviceContext* context,SenderCoverage& coverage){
    static const char shader[]=R"(
cbuffer Color : register(b0) { float4 color; };
float4 vs_main(uint id:SV_VertexID):SV_Position { return float4(float2(float(id&1u),float(id>>1u))*2.0-1.0,0.0,1.0); }
float4 ps_main():SV_Target { return color; }
)";
    Com<ID3DBlob> vsBytes,psBytes;
    check(SUCCEEDED(D3DCompile(shader,sizeof shader,nullptr,nullptr,nullptr,"vs_main","vs_5_0",0,0,vsBytes.out(),nullptr)),"own representative native-like VS compiled");
    check(SUCCEEDED(D3DCompile(shader,sizeof shader,nullptr,nullptr,nullptr,"ps_main","ps_5_0",0,0,psBytes.out(),nullptr)),"own representative native-like PS compiled");
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;
    check(SUCCEEDED(device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,vs.out()))&&
        SUCCEEDED(device->CreatePixelShader(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),nullptr,ps.out())),"real native shader objects created");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Buffer> cb;check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,cb.out())),"real native mapped constant buffer");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> target,staging;Com<ID3D11RenderTargetView> view;
    check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,target.out()))&&SUCCEEDED(device->CreateRenderTargetView(target.p,nullptr,view.out())),"real native RGB target");
    td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.BindFlags=0;
    check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,staging.out())),"real native RGB verification target");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    Com<ID3D11RasterizerState> rs;check(SUCCEEDED(device->CreateRasterizerState(&raster,rs.out())),"real native rasterizer state");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;sampler.MaxLOD=D3D11_FLOAT32_MAX;Com<ID3D11SamplerState> sample;
    check(SUCCEEDED(device->CreateSamplerState(&sampler,sample.out())),"real native sampler state");
    const auto before=coverage.gateStats().hookDraws;const float color[4]{17.0f/255,29.0f/255,43.0f/255,101.0f/255};
    for(unsigned frame=0;frame<32;++frame){
        context->ClearState();
        context->VSSetShader(vs.p,nullptr,0);context->PSSetShader(ps.p,nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);context->RSSetState(rs.p);
        const D3D11_VIEWPORT viewport{0,0,4,4,0,1};context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&view.p,nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(cb.p,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)),"real native constant update Map forwarded");
        std::memcpy(mapped.pData,color,sizeof color);context->Unmap(cb.p,0);context->PSSetConstantBuffers(0,1,&cb.p);context->PSSetSamplers(0,1,&sample.p);
        if(frame==0)diagnostic("after native-like pipeline bindings",coverage.gateStats(),true);
        context->Draw(4,0);auto stats=coverage.gateStats();
        if(stats.hookDraws!=before+frame+1||stats.contextHookSlots!=15)diagnostic("lost after native-like Draw",stats,true);
        check(stats.hookDraws==before+frame+1&&stats.contextHookSlots==15,"all real rendering frames retain production hooks after native state changes");
        context->CopyResource(staging.p,target.p);check(SUCCEEDED(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped)),"real native rendered pixels mapped");
        const auto* pixel=static_cast<const unsigned char*>(mapped.pData);
        check(pixel&&pixel[0]==17&&pixel[1]==29&&pixel[2]==43&&pixel[3]==101,"ordinary native RGB and fractional alpha unchanged by observer");context->Unmap(staging.p,0);
        // The real pinned native PostPresent path queries its device's public
        // immediate context, flushes and releases it before encoder Copy.
        Com<ID3D11DeviceContext> post;device->GetImmediateContext(post.out());post->Flush();
        stats=coverage.gateStats();check(stats.contextHookSlots==15,"native PostPresent getter and Flush preserve dispatch");
    }
    check(coverage.gateStats().sourceFrames==0&&coverage.gateStats().certifiedMasks==0,"ordinary native-like frames never fabricate attested source coverage");
}
// This is deliberately a real native context, not a fake COM object. The
// public CreateDevice output is retained on the rendering thread while the
// production observer obtains GetImmediateContext and installs on its worker.
void run(bool changeProtection){
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
    diagnostic("after installation",coverage.gateStats(),true);
    auto** const installed=*reinterpret_cast<void***>(submittedContext.p);
    check(installed[13]!=drawBefore&&installed[14]!=mapBefore&&installed[15]!=unmapBefore&&installed[47]!=copyBefore,"all four actual CreateDevice-context dispatch slots intercepted");
    submittedContext->Draw(0,0);
    auto stats=coverage.gateStats();
    diagnostic("after first Draw",stats,true);
    check(stats.contextTableReadable&&stats.contextTableStable&&stats.contextHookSlots==15,"cold production snapshot verifies actual current dispatch without publishing addresses");
    check(stats.hookDraws==1&&stats.otherDraws==1&&stats.nativeDraws==0,"CreateDevice output Draw reaches production hook on rendering thread");
    check(stats.sourceFrames==0&&stats.certifiedMasks==0,"fixture call cannot authorize a fabricated native source");
    Com<ID3D11DeviceContext> queried;std::thread queryWorker([&]{device->GetImmediateContext(queried.out());});queryWorker.join();
    queried->Draw(0,0);
    stats=coverage.gateStats();
    diagnostic("after queried Draw",stats,queried.p==submittedContext.p);
    check(stats.hookDraws==2,"cold GetImmediateContext alias reaches the same production observer");
    Com<ID3D11DeviceContext1> extended;
    check(SUCCEEDED(submittedContext->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(extended.out()))),"native versioned context interface");
    extended->Draw(0,0);
    check(coverage.gateStats().hookDraws==3,"versioned interface dispatch reaches production hook");
    submittedContext->ClearState();diagnostic("after ClearState",coverage.gateStats(),true);
    submittedContext->Draw(0,0);diagnostic("after ClearState Draw",coverage.gateStats(),true);
    check(coverage.gateStats().hookDraws==4,"native ClearState retains dispatch hooks");
    pipeline(device.p,submittedContext.p,coverage);
    if(changeProtection){
        check(threading->SetMultithreadProtected(FALSE)==TRUE&&!threading->GetMultithreadProtected(),"native protection disable keeps original return value and mode");
        diagnostic("after protection FALSE",coverage.gateStats(),true);coverage.tick();
        check(std::strcmp(coverage.stateName(),"coverage-public-context-dispatch-changed")==0,"cold production tick explicitly rejects changed public dispatch");
        check(!coverage.ready(1)&&!coverage.operational(1),"changed dispatch revokes source grant and presentation readiness");
        check(threading->SetMultithreadProtected(TRUE)==FALSE&&threading->GetMultithreadProtected(),"native protection enable keeps original return value and mode");
        submittedContext->Draw(0,0);coverage.tick();diagnostic("after protection TRUE Draw",coverage.gateStats(),true);
        check(std::strcmp(coverage.stateName(),"coverage-public-context-dispatch-changed")==0&&!coverage.ready(1)&&!coverage.operational(1),"later mode changes never silently reinstall or restore prior grants");
    }
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
    if(changeProtection){
        void** live=*reinterpret_cast<void***>(submittedContext.p);
        const std::array<void*,4> native{{live[13],live[14],live[15],live[47]}};
        coverage.stop();
        check(live[13]==native[0]&&live[14]==native[1]&&live[15]==native[2]&&live[47]==native[3],"stop never overwrites native dispatch after a protection-mode change");
        check(coverage.gateStats().sourceFrames==0&&coverage.gateStats().certifiedMasks==0,"unsupported protection transition never fabricates frame authority");
        return;
    }
    // A later independent observer owns its replacement. Cleanup must not
    // overwrite it even while restoring the other three original slots.
    void** const live=*reinterpret_cast<void***>(submittedContext.p);void* const installedUnmap=live[15];
    otherPreviousUnmap=pointerBits<NativeUnmap>(installedUnmap);
    replace(live+15,installedUnmap,pointerBits<void*>(&otherUnmap));
    coverage.stop();
    check(live[15]==pointerBits<void*>(&otherUnmap)&&coverage.gateStats().contextOriginalSlots==11,"production stop preserves a later independent hook while restoring only its own slots");
    replace(live+15,pointerBits<void*>(&otherUnmap),installedUnmap);coverage.stop();
    stats=coverage.gateStats();diagnostic("after stop",stats,true);
    check(stats.contextTableReadable&&stats.contextTableStable&&stats.contextHookSlots==0&&stats.contextOriginalSlots==15,"production stop restores the exact native slots recorded after public initialization");
}
}
int main(int argc,char** argv){try{
const bool changed=argc==2&&std::strcmp(argv[1],"--protection-change")==0;
check(argc==1||changed,"fixture arguments are explicit");run(changed);std::printf("production native D3D11 context observer: %u checks PASS\n",checks);return 0;}
catch(const std::exception& e){std::fprintf(stderr,"native context observer failed after %u: %s\n",checks,e.what());return 1;}}
