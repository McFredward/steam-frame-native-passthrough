// Narrow feasibility experiment, not a production provider or source authority.
// The only new executable bytes are our public-ABI forwarding thunks. No Valve
// binary bytes, disassembly or shader code are inputs to this fixture.
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <numeric>
#include <cstring>
#include <stdexcept>
#include <vector>
extern "C" std::size_t sfnp_fixture_context_slots(unsigned);
namespace {
unsigned checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->()const{return p;}};
template<class To,class From>To bits(From value){static_assert(sizeof(To)==sizeof(From),"Win64 pointer size");To out{};std::memcpy(&out,&value,sizeof out);return out;}
struct Unsupported:std::runtime_error{using std::runtime_error::runtime_error;};
using Draw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using Map=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using Unmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
struct Shadow;
Shadow* active=nullptr;
void STDMETHODCALLTYPE draw(ID3D11DeviceContext*,UINT,UINT);
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext*,ID3D11Resource*,UINT);
void STDMETHODCALLTYPE copy(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
struct Shadow {
    ID3D11DeviceContext* context;
    void **original,**table=nullptr,**foreign=nullptr;
    unsigned char* thunks=nullptr;
    std::size_t count;
    std::vector<void*> initial,expected;
    std::array<unsigned,4> calls{};
    bool forwardingValid=true;
    unsigned nativeChanges=0;
    Shadow(ID3D11DeviceContext* value,std::size_t slots):context(value),original(*reinterpret_cast<void***>(value)),count(slots),initial(original,original+count){
        check(count>47&&count<256,"complete public SDK table extent bounded");
        table=static_cast<void**>(VirtualAlloc(nullptr,count*sizeof(void*),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        foreign=static_cast<void**>(VirtualAlloc(nullptr,count*sizeof(void*),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        thunks=static_cast<unsigned char*>(VirtualAlloc(nullptr,count*16,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        check(table&&foreign&&thunks,"allocate full RW table and owned thunk code");
        for(std::size_t slot=0;slot<count;++slot){
            check(original[slot]!=nullptr,"all complete public table entries readable and nonnull");
            // mov rax, address-of-current-original-slot; jmp qword ptr [rax].
            // A tail-jump preserves ABI stack arguments and arithmetic flags.
            auto* code=thunks+slot*16;code[0]=0x48;code[1]=0xb8;
            const auto location=bits<std::uintptr_t>(original+slot);std::memcpy(code+2,&location,sizeof location);code[10]=0xff;code[11]=0x20;
            table[slot]=bits<void*>(code);
        }
        table[13]=bits<void*>(&draw);table[14]=bits<void*>(&map);table[15]=bits<void*>(&unmap);table[47]=bits<void*>(&copy);
        expected.assign(table,table+count);std::copy(expected.begin(),expected.end(),foreign);
        DWORD old=0;check(VirtualProtect(thunks,count*16,PAGE_EXECUTE_READ,&old)!=0&&old==PAGE_READWRITE,"own thunk pages become RX, never writable executable");
        check(FlushInstructionCache(GetCurrentProcess(),thunks,count*16)!=0,"owned thunk cache publication");
        MEMORY_BASIC_INFORMATION info{};check(VirtualQuery(table,&info,sizeof info)==sizeof info&&info.Protect==PAGE_READWRITE,"full context shadow intentionally remains writable");
        active=this;
        check(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(context),table,original)==original,"single aligned 8-byte object vptr CAS publishes full shadow");
        check(std::equal(initial.begin(),initial.end(),original),"publication never changes original native table slots");
    }
    // Buffers deliberately remain allocated through process exit: even a failed
    // restore/foreign owner must never leave a dangling callback/table pointer.
    bool restore(){return InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(context),original,table)==table;}
    template<class Function>Function current(std::size_t slot){
        void* value=*reinterpret_cast<void* volatile*>(original+slot);
        forwardingValid=forwardingValid&&value&&value!=table[slot];
        return value&&value!=table[slot]?bits<Function>(value):nullptr;
    }
    void inspect(const char* phase){
        const bool same=*reinterpret_cast<void***>(context)==table;
        const auto changed=static_cast<unsigned>(std::inner_product(initial.begin(),initial.end(),original,0u,std::plus<unsigned>(),std::not_equal_to<void*>()));
        nativeChanges=std::max(nativeChanges,changed);
        bool shadowSame=std::equal(expected.begin(),expected.end(),table);
        MEMORY_BASIC_INFORMATION info{};const bool writable=VirtualQuery(table,&info,sizeof info)==sizeof info&&info.Protect==PAGE_READWRITE;
        if(!same||!shadowSame||!writable)std::printf("shadow diagnostic %s: same-vptr=%u, full-shadow-same=%u, RW=%u, original-native-slots-changed=%u\n",phase,unsigned(same),unsigned(shadowSame),unsigned(writable),changed);
        check(same&&shadowSame&&writable,"runtime must retain writable object shadow and every entry");
        check(forwardingValid,"dynamic native forwarding never recurses into a shadow hook");
    }
};
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* context,UINT count,UINT start){++active->calls[0];auto call=active->current<Draw>(13);if(call)call(context,count,start);}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE* out){++active->calls[1];auto call=active->current<Map>(14);return call?call(context,resource,sub,mode,flags,out):E_FAIL;}
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub){++active->calls[2];auto call=active->current<Unmap>(15);if(call)call(context,resource,sub);}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context,ID3D11Resource* destination,ID3D11Resource* source){++active->calls[3];auto call=active->current<Copy>(47);if(call)call(context,destination,source);}
template<class Context>bool alias(ID3D11DeviceContext* context,unsigned version){
    Com<Context> queried;const HRESULT status=context->QueryInterface(__uuidof(Context),reinterpret_cast<void**>(queried.out()));
    if(status==E_NOINTERFACE){std::printf("public context version %u unavailable\n",version);return false;}
    check(SUCCEEDED(status)&&queried.p,"public context QueryInterface returns valid interface");
    if(bits<void*>(queried.p)!=bits<void*>(context))throw Unsupported("successful public context version has different interface pointer; no table length guessed");
    check(queried->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"versioned interface remains callable through full shadow");return true;
}
unsigned highest(ID3D11DeviceContext* context){unsigned result=0;
    if(alias<ID3D11DeviceContext1>(context,1))result=1;
    if(alias<ID3D11DeviceContext2>(context,2))result=2;
    if(alias<ID3D11DeviceContext3>(context,3))result=3;
    if(alias<ID3D11DeviceContext4>(context,4))result=4;
    return result;
}
void pipeline(ID3D11Device* device,ID3D11DeviceContext* context,Shadow& shadow,const char* phase){
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
        if(frame==0||frame==31){std::printf("writable context shadow %s frame %u: slots=%zu, native-changed=%u, draws=%u maps=%u unmaps=%u copies=%u\n",phase,frame,shadow.count,shadow.nativeChanges,shadow.calls[0],shadow.calls[1],shadow.calls[2],shadow.calls[3]);std::fflush(stdout);}
    }
}
void run(){
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    const HRESULT created=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,context.out());
    if(FAILED(created)||level<D3D_FEATURE_LEVEL_11_0)throw Unsupported("actual native WARP D3D11 unavailable");
    Com<ID3D11Multithread> threading;check(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(threading.out()))),"public multithread interface");threading->SetMultithreadProtected(TRUE);
    context->Draw(0,0);context->Flush(); // Explicit native initialization before publication.
    const unsigned version=highest(context.p);const std::size_t slots=sfnp_fixture_context_slots(version);
    std::printf("public highest same-pointer context version=%u, CINTERFACE SDK slots=%zu; explicit native cold Draw+Flush completed\n",version,slots);
    Shadow shadow(context.p,slots);shadow.inspect("initial publication");context->Draw(0,0);shadow.inspect("first shadowed zero Draw");
    pipeline(device.p,context.p,shadow,"protected");
    check(highest(context.p)==version,"all supported public interface aliases survive full shadow publication");shadow.inspect("after versioned QueryInterface getters");
    check(threading->SetMultithreadProtected(FALSE)==TRUE&&!threading->GetMultithreadProtected(),"native protection FALSE preserves result and mode");shadow.inspect("after protection FALSE");
    pipeline(device.p,context.p,shadow,"unprotected");
    check(threading->SetMultithreadProtected(TRUE)==FALSE&&threading->GetMultithreadProtected(),"native protection TRUE preserves result and mode");shadow.inspect("after protection TRUE");
    pipeline(device.p,context.p,shadow,"protected-again");
    check(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(context.p),shadow.foreign,shadow.table)==shadow.table,"fixture publishes independent later object owner");
    check(!shadow.restore()&&*reinterpret_cast<void***>(context.p)==shadow.foreign,"exact restore preserves independent foreign object vptr");
    check(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(context.p),shadow.table,shadow.foreign)==shadow.foreign,"fixture releases its later foreign owner");
    check(shadow.restore()&&*reinterpret_cast<void***>(context.p)==shadow.original,"single-pointer exact owned restoration");
    check(shadow.forwardingValid,"no recursive forward during entire experiment");context->Draw(0,0);context->Flush();
    std::printf("native mutable original maximum changed slots=%u; original table never written by fixture; owned buffers retained until process exit\n",shadow.nativeChanges);
}
}
int main(){try{run();std::printf("experimental writable complete context shadow: %u checks PASS; no native source/frame/passthrough authority claimed\n",checks);return 0;}catch(const Unsupported& e){std::fprintf(stderr,"writable shadow capability UNSUPPORTED: %s\n",e.what());return 77;}catch(const std::exception& e){std::fprintf(stderr,"writable shadow FAILED after %u: %s\n",checks,e.what());return 1;}}
