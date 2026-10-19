#include "pc/stream-bridge-sender-provider-coverage.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <numeric>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <thread>
extern "C" std::size_t sfnp_fixture_context_slots(unsigned);
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
    std::printf("context diagnostic %s: draws=%llu, other=%llu, same-created=%u, readable=%u, stable=%u, native-device-same=%u, slots=%u, originals=%u, facade-public-slots=%u, native-mutations=%u\n",
        phase,static_cast<unsigned long long>(stats.hookDraws),static_cast<unsigned long long>(stats.otherDraws),unsigned(sameCreated),
        unsigned(stats.contextTableReadable),unsigned(stats.contextTableStable),unsigned(stats.nativeDeviceSame),stats.contextHookSlots,stats.contextOriginalSlots,stats.contextFacadeSlots,stats.nativeDispatchMutations);
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
        if(stats.hookDraws!=before+2*frame+1||stats.contextHookSlots!=15)diagnostic("lost after native-like Draw",stats,true);
        check(stats.hookDraws==before+2*frame+1&&stats.contextHookSlots==15,"all real rendering frames retain production hooks after native state changes");
        context->CopyResource(staging.p,target.p);check(SUCCEEDED(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped)),"real native rendered pixels mapped");
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto* pixel=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
            check(pixel&&pixel[0]==17&&pixel[1]==29&&pixel[2]==43&&pixel[3]==101,"all ordinary native RGB pixels and fractional alpha unchanged by observer");}
        context->Unmap(staging.p,0);
        // The real pinned native PostPresent path queries its device's public
        // immediate context, flushes and releases it before encoder Copy.
        Com<ID3D11DeviceContext> post;
        if(frame==0)diagnostic("before native PostPresent getter",coverage.gateStats(),true);
        device->GetImmediateContext(post.out());
        if(frame==0)diagnostic("after native PostPresent getter",coverage.gateStats(),post.p==context);
        post->Flush();stats=coverage.gateStats();
        if(frame==0||stats.contextHookSlots!=15)diagnostic("after native PostPresent Flush",stats,post.p==context);
        post->Draw(0,0);stats=coverage.gateStats();
        if(frame==0||stats.contextHookSlots!=15)diagnostic("after native PostPresent next Draw",stats,post.p==context);
        check(stats.contextHookSlots==15&&stats.hookDraws==before+2*frame+2,"native PostPresent getter, Flush and following Draw preserve dispatch");
    }
    check(coverage.gateStats().sourceFrames==0&&coverage.gateStats().certifiedMasks==0,"ordinary native-like frames never fabricate attested source coverage");
}

using ProbeDraw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using ProbeMap=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using ProbeUnmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using ProbeCopy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
struct FacadeProbe;
FacadeProbe* probe=nullptr;
void STDMETHODCALLTYPE probeDraw(ID3D11DeviceContext*,UINT,UINT);
HRESULT STDMETHODCALLTYPE probeMap(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
void STDMETHODCALLTYPE probeUnmap(ID3D11DeviceContext*,ID3D11Resource*,UINT);
void STDMETHODCALLTYPE probeCopy(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
struct FacadeProbe{
    CoverageContextFacade facade;ID3D11DeviceContext* context;void** original;void** table=nullptr;
    std::vector<void*> initial,expected;std::array<unsigned,4> calls{};unsigned count,nativeChanges=0;bool forwardingValid=true;
    FacadeProbe(ID3D11DeviceContext* value,unsigned slots):context(value),original(*reinterpret_cast<void***>(value)),initial(original,original+slots),count(slots){
        probe=this;const std::array<std::uintptr_t,4> callbacks{{pointerBits<std::uintptr_t>(&probeDraw),pointerBits<std::uintptr_t>(&probeMap),pointerBits<std::uintptr_t>(&probeUnmap),pointerBits<std::uintptr_t>(&probeCopy)}};
        check(facade.prepare(value,callbacks)&&facade.publish(),"actual production facade publishes before any native Draw/Flush warmup");
        check(facade.slotCount()==slots,"actual production facade extent equals highest same-pointer SDK CINTERFACE ABI");
        table=*reinterpret_cast<void***>(context);expected.assign(table,table+count);
        check(table!=original&&std::equal(initial.begin(),initial.end(),original),"production publication leaves original system table unchanged");
    }
    template<class Function>Function current(unsigned slot){const auto target=facade.originalFunction(slot);forwardingValid=forwardingValid&&target&&target!=pointerBits<std::uintptr_t>(table[slot]);return forwardingValid?pointerBits<Function>(target):nullptr;}
    void inspect(const char* phase){
        MEMORY_BASIC_INFORMATION info{};const bool rw=VirtualQuery(table,&info,sizeof info)==sizeof info&&info.Protect==PAGE_READWRITE;
        const bool same=*reinterpret_cast<void***>(context)==table&&std::equal(expected.begin(),expected.end(),table);
        nativeChanges=std::max(nativeChanges,facade.nativeMutationCount());
        if(!rw||!same||!facade.installed())std::printf("production facade diagnostic %s: RW=%u, same-vptr-and-table=%u, installed=%u\n",phase,unsigned(rw),unsigned(same),unsigned(facade.installed()));
        check(rw&&same&&facade.installed(),"real native work retains full writable production facade");
        for(unsigned slot=0;slot<count;++slot){
            const bool fresh=facade.originalFunction(slot)==pointerBits<std::uintptr_t>(original[slot]);
            if(!fresh)std::printf("STALE NATIVE TARGET after %s: public-slot=%u, native-mutations=%u\n",phase,slot,facade.nativeMutationCount());
            check(fresh,"production getter follows current mutable native table, never initial snapshots");
        }
        check(forwardingValid,"production callback forwarding never recurses");
    }
};
void STDMETHODCALLTYPE probeDraw(ID3D11DeviceContext* context,UINT count,UINT start){++probe->calls[0];auto call=probe->current<ProbeDraw>(13);if(call)call(context,count,start);}
HRESULT STDMETHODCALLTYPE probeMap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE* out){++probe->calls[1];auto call=probe->current<ProbeMap>(14);return call?call(context,resource,sub,mode,flags,out):E_FAIL;}
void STDMETHODCALLTYPE probeUnmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub){++probe->calls[2];auto call=probe->current<ProbeUnmap>(15);if(call)call(context,resource,sub);}
void STDMETHODCALLTYPE probeCopy(ID3D11DeviceContext* context,ID3D11Resource* destination,ID3D11Resource* source){++probe->calls[3];auto call=probe->current<ProbeCopy>(47);if(call)call(context,destination,source);}
template<class Interface>bool probeAlias(ID3D11DeviceContext* context){Com<Interface> value;const HRESULT result=context->QueryInterface(__uuidof(Interface),reinterpret_cast<void**>(value.out()));if(result==E_NOINTERFACE)return false;check(SUCCEEDED(result)&&pointerBits<void*>(value.p)==pointerBits<void*>(context),"supported public ContextN must retain same interface pointer");check(value->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"versioned public context methods retain ABI");return true;}
unsigned probeVersion(ID3D11DeviceContext* context){unsigned version=0;if(probeAlias<ID3D11DeviceContext1>(context))version=1;if(probeAlias<ID3D11DeviceContext2>(context))version=2;if(probeAlias<ID3D11DeviceContext3>(context))version=3;if(probeAlias<ID3D11DeviceContext4>(context))version=4;return version;}
void facadePipeline(ID3D11Device* device,ID3D11DeviceContext* context,FacadeProbe& shadow,const char* phase){
    static const char shader[]=R"(
cbuffer Color : register(b0) { float4 color; };
float4 vs_main(uint id:SV_VertexID):SV_Position { return float4(float2(float(id&1u),float(id>>1u))*2.0-1.0,0.0,1.0); }
float4 ps_main():SV_Target { return color; }
)";
    Com<ID3DBlob> vsBytes,psBytes;
    check(SUCCEEDED(D3DCompile(shader,sizeof shader,nullptr,nullptr,nullptr,"vs_main","vs_5_0",0,0,vsBytes.out(),nullptr))&&SUCCEEDED(D3DCompile(shader,sizeof shader,nullptr,nullptr,nullptr,"ps_main","ps_5_0",0,0,psBytes.out(),nullptr)),"compile own representative native-like shader");
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;
    check(SUCCEEDED(device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,vs.out()))&&SUCCEEDED(device->CreatePixelShader(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),nullptr,ps.out())),"real WARP shader objects");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Buffer> cb;check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,cb.out())),"real mapped constant buffer");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> target,staging;Com<ID3D11RenderTargetView> view;
    check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,target.out()))&&SUCCEEDED(device->CreateRenderTargetView(target.p,nullptr,view.out())),"native-like rendered target");
    td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.BindFlags=0;check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,staging.out())),"real readback staging");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    Com<ID3D11RasterizerState> rs;check(SUCCEEDED(device->CreateRasterizerState(&raster,rs.out())),"native rasterizer");
    for(unsigned frame=0;frame<32;++frame){
        shadow.inspect("before native state bindings");const auto before=shadow.calls;
        context->ClearState();context->VSSetShader(vs.p,nullptr,0);context->PSSetShader(ps.p,nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);context->RSSetState(rs.p);
        const D3D11_VIEWPORT viewport{0,0,4,4,0,1};context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&view.p,nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(cb.p,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)),"native constant Map through current original slot");
        const unsigned char red=static_cast<unsigned char>(17+frame);const float color[4]{float(red)/255,29.0f/255,43.0f/255,101.0f/255};std::memcpy(mapped.pData,color,sizeof color);context->Unmap(cb.p,0);context->PSSetConstantBuffers(0,1,&cb.p);
        context->Draw(4,0);shadow.inspect("after actual shader Draw");context->CopyResource(staging.p,target.p);shadow.inspect("after actual CopyResource");
        check(SUCCEEDED(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped)),"blocking native pixel readback");shadow.inspect("after blocking readback Map");
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto* pixel=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
            check(pixel[0]==red&&pixel[1]==29&&pixel[2]==43&&pixel[3]==101,"all original RGB pixels and fractional alpha unchanged");}
        context->Unmap(staging.p,0);shadow.inspect("after native Unmap");
        Com<ID3D11DeviceContext> queried;device->GetImmediateContext(queried.out());check(queried.p==context,"GetImmediateContext keeps public object identity");shadow.inspect("after native context getter");
        queried->Flush();shadow.inspect("after native Flush");queried->Draw(0,0);shadow.inspect("after post-Flush Draw");
        check(shadow.calls[0]==before[0]+2&&shadow.calls[1]==before[1]+2&&shadow.calls[2]==before[2]+2&&shadow.calls[3]==before[3]+1,"all callbacks remain active across native real work");
        if(frame==0||frame==31){std::printf("writable context shadow %s frame %u: slots=%u, native-changed=%u, draws=%u maps=%u unmaps=%u copies=%u\n",phase,frame,shadow.count,shadow.nativeChanges,shadow.calls[0],shadow.calls[1],shadow.calls[2],shadow.calls[3]);std::fflush(stdout);}
    }
}
void runFacade(){
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,context.out()))&&level>=D3D_FEATURE_LEVEL_11_0,"actual production-facade WARP device");
    Com<ID3D11Multithread> threading;check(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(threading.out()))),"public native threading interface");threading->SetMultithreadProtected(TRUE);
    const unsigned version=probeVersion(context.p);FacadeProbe shadow(context.p,static_cast<unsigned>(sfnp_fixture_context_slots(version)));
    context->Draw(0,0);shadow.inspect("first cold Draw without warmup");
    facadePipeline(device.p,context.p,shadow,"production-protected");
    check(probeVersion(context.p)==version,"all public aliases survive actual production facade");
    check(threading->SetMultithreadProtected(FALSE)==TRUE&&!threading->GetMultithreadProtected(),"actual native protection FALSE");shadow.inspect("native protection FALSE");
    facadePipeline(device.p,context.p,shadow,"production-unprotected");
    check(threading->SetMultithreadProtected(TRUE)==FALSE&&threading->GetMultithreadProtected(),"actual native protection TRUE");shadow.inspect("native protection TRUE");
    facadePipeline(device.p,context.p,shadow,"production-protected-again");
    check(shadow.nativeChanges>0,"real native runtime mutated its original dispatch and exercised dynamic-forwarding control");
    const auto own=shadow.table[15];otherPreviousUnmap=pointerBits<NativeUnmap>(own);replace(shadow.table+15,own,pointerBits<void*>(&otherUnmap));
    const std::vector<void*> foreignEntries(shadow.table,shadow.table+shadow.count);
    check(!shadow.facade.restore()&&*reinterpret_cast<void***>(context.p)==shadow.table&&std::equal(foreignEntries.begin(),foreignEntries.end(),shadow.table),"actual production restore preserves entire independently modified facade");
    replace(shadow.table+15,pointerBits<void*>(&otherUnmap),own);
    auto** foreign=static_cast<void**>(VirtualAlloc(nullptr,shadow.count*sizeof(void*),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));check(foreign!=nullptr,"own full foreign table allocation");std::copy(shadow.table,shadow.table+shadow.count,foreign);
    check(InterlockedCompareExchangePointer(reinterpret_cast<void**>(context.p),foreign,shadow.table)==shadow.table,"fixture publishes independent foreign object owner");
    check(!shadow.facade.restore()&&*reinterpret_cast<void***>(context.p)==foreign,"actual production restore preserves foreign object vptr");
    check(InterlockedCompareExchangePointer(reinterpret_cast<void**>(context.p),shadow.table,foreign)==foreign,"fixture releases foreign owner");
    check(shadow.facade.restore()&&!shadow.facade.installed()&&*reinterpret_cast<void***>(context.p)==shadow.original,"actual production exact vptr restore clears facade authority");
    check(shadow.facade.restore(),"actual production restore is idempotent");
    context->Draw(0,0);context->Flush();
    std::printf("actual production facade complete: slots=%u, native-original-mutations=%u, no fabricated source/frame permission\n",shadow.count,shadow.nativeChanges);
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
    check(installed!=before&&installed[13]!=drawBefore&&installed[14]!=mapBefore&&installed[15]!=unmapBefore&&installed[47]!=copyBefore,"all four callbacks are object-local, not original native table mutations");
    check(before[13]==drawBefore&&before[14]==mapBefore&&before[15]==unmapBefore&&before[47]==copyBefore,"publication leaves original native entries intact before first cold Draw");
    // No Draw/Flush warmup has run before production publication.
    submittedContext->Draw(0,0);
    auto stats=coverage.gateStats();
    diagnostic("after first Draw",stats,true);
    check(stats.contextTableReadable&&stats.contextTableStable&&stats.contextHookSlots==15,"cold production facade verifies first Draw without any warmup");
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
        const auto refusalsBefore=coverage.gateStats().refusals[static_cast<unsigned>(CoverageGate::Context)];
        diagnostic("after protection FALSE",coverage.gateStats(),true);coverage.tick();
        check(std::strcmp(coverage.stateName(),"coverage-public-context-dispatch-changed")==0,"cold production tick explicitly rejects changed public dispatch");
        check(coverage.gateStats().refusals[static_cast<unsigned>(CoverageGate::Context)]>refusalsBefore,"changed public mode records the actual dispatch-contract refusal");
        // This fixture never had a valid source/process grant. ready(false) is
        // only a no-fabrication check, not proof of revoking a former grant.
        check(!coverage.ready(1)&&!coverage.operational(1),"ordinary fixture never fabricates source or presentation authority");
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
        check(*reinterpret_cast<void***>(submittedContext.p)==before&&live[13]==native[0]&&live[14]==native[1]&&live[15]==native[2]&&live[47]==native[3],"stop restores owned vptr and leaves retained facade unchanged after protection-mode change");
        check(coverage.gateStats().sourceFrames==0&&coverage.gateStats().certifiedMasks==0,"unsupported protection transition never fabricates frame authority");
        return;
    }
    // A later independent observer owns its replacement. Cleanup must not
    // overwrite its whole object-local facade or any of the other entries.
    void** const live=*reinterpret_cast<void***>(submittedContext.p);void* const installedUnmap=live[15];
    otherPreviousUnmap=pointerBits<NativeUnmap>(installedUnmap);
    replace(live+15,installedUnmap,pointerBits<void*>(&otherUnmap));
    const std::vector<void*> foreignEntries(live,live+coverage.gateStats().contextFacadeSlots);
    coverage.stop();
    check(std::equal(foreignEntries.begin(),foreignEntries.end(),live),"production stop leaves every entry of foreign-modified facade unchanged");
    check(*reinterpret_cast<void***>(submittedContext.p)==live&&live[15]==pointerBits<void*>(&otherUnmap)&&coverage.gateStats().contextHookSlots==11,"production stop preserves entire foreign-modified facade and its independent hook");
    replace(live+15,pointerBits<void*>(&otherUnmap),installedUnmap);
    auto** foreign=static_cast<void**>(VirtualAlloc(nullptr,coverage.gateStats().contextFacadeSlots*sizeof(void*),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    check(foreign!=nullptr,"fixture allocates independent complete foreign object table");
    std::copy(live,live+coverage.gateStats().contextFacadeSlots,foreign);
    check(InterlockedCompareExchangePointer(reinterpret_cast<void**>(submittedContext.p),foreign,live)==live,"fixture adopts a later foreign object vptr");
    coverage.stop();check(*reinterpret_cast<void***>(submittedContext.p)==foreign,"production stop never overwrites a later foreign object vptr");
    check(InterlockedCompareExchangePointer(reinterpret_cast<void**>(submittedContext.p),live,foreign)==foreign,"fixture releases its later object owner");
    coverage.stop();
    stats=coverage.gateStats();diagnostic("after stop",stats,true);
    check(*reinterpret_cast<void***>(submittedContext.p)==before&&stats.contextTableReadable&&!stats.contextTableStable&&stats.contextHookSlots==0&&stats.contextOriginalSlots==15,"production stop CAS restores current native object vptr after foreign owners release it");
}
}
int main(int argc,char** argv){try{
const bool changed=argc==2&&std::strcmp(argv[1],"--protection-change")==0;
const bool facade=argc==2&&std::strcmp(argv[1],"--facade")==0;
check(argc==1||changed||facade,"fixture arguments are explicit");if(facade)runFacade();else run(changed);std::printf("production native D3D11 context observer: %u checks PASS\n",checks);return 0;}
catch(const std::exception& e){std::fprintf(stderr,"native context observer failed after %u: %s\n",checks,e.what());return 1;}}
