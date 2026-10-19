// Public DXGI sharing contract only; no Valve/SDK-private data or Steam process.
#include "pc/stream-bridge-sender-provider-coverage-render.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class T>struct Com{T* p=nullptr;~Com(){reset();}T** out(){return &p;}void reset(){if(p){p->Release();p=nullptr;}}T* operator->(){return p;}};
struct Texture;
struct Shared final:IDXGIResource{
    Texture& owner;explicit Shared(Texture& v):owner(v){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**)override;
    ULONG STDMETHODCALLTYPE AddRef()override;
    ULONG STDMETHODCALLTYPE Release()override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetParent(REFIID,void**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE*)override;
    HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT*)override{return E_NOTIMPL;}
};
struct Texture final:ID3D11Texture2D{
    ULONG references=1;Shared shared{*this};std::uint64_t handle=0;HRESULT sharing=S_OK;bool texture=true,dxgi=true;
    D3D11_TEXTURE2D_DESC desc{};
    explicit Texture(std::uint64_t value):handle(value){desc.Width=32;desc.Height=16;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=desc.ArraySize=desc.MipLevels=1;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result)override{
        if(!result)return E_POINTER;
        *result=nullptr;
        if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_ID3D11Resource)||(IsEqualIID(id,IID_ID3D11Texture2D)&&texture))*result=static_cast<ID3D11Texture2D*>(this);
        else if(IsEqualIID(id,IID_IDXGIResource)&&dxgi)*result=static_cast<IDXGIResource*>(&shared);
        if(!*result)return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{return --references;}
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out)override{if(out)*out=nullptr;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*)override{return E_NOTIMPL;}
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* out)override{if(out)*out=D3D11_RESOURCE_DIMENSION_TEXTURE2D;}
    void STDMETHODCALLTYPE SetEvictionPriority(UINT)override{}
    UINT STDMETHODCALLTYPE GetEvictionPriority()override{return 0;}
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* out)override{if(out)*out=desc;}
};
HRESULT Shared::QueryInterface(REFIID id,void** out){return owner.QueryInterface(id,out);}
ULONG Shared::AddRef(){return owner.AddRef();}ULONG Shared::Release(){return owner.Release();}
HRESULT Shared::GetSharedHandle(HANDLE* out){if(!out)return E_POINTER;*out=SUCCEEDED(owner.sharing)?reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(owner.handle)):nullptr;return owner.sharing;}
void metadata(){
    Texture native(0x801),held(0x801),foreign(0x802);
    const SharedTextureOwner key{0x801,17,290};SharedTextureShape shape{};
    check(decodePinnedSwapShape(32,16,DXGI_FORMAT_R8G8B8A8_UNORM,9,shape)&&shape.samples==1&&shape.arraySize==1,"actual packed descriptor 9 normalizes to single-sample single-array coverage shape");
    check(sharedTextureDescriptionMatches(native.desc,shape),"production cold import matches normalized descriptor 9 to actual kernel texture description");
    check(static_cast<IUnknown*>(&native)!=static_cast<IUnknown*>(&held),"fixture uses distinct COM objects, not one reused resource pointer");
    check(submittedSharedResource(&native,&held,key,key,shape),"separately opened SAME allocation admitted by explicit shared handle");
    check(submittedSharedResource(&native,&native,key,key,shape),"same wrapper is also valid with actual shared handle proof");
    check(!submittedSharedResource(&foreign,&held,key,key,shape),"same size and format NEVER admit a foreign native allocation");
    check(!submittedSharedResource(&native,&foreign,key,key,shape),"held open must independently prove same allocation");
    for(auto bad:{SharedTextureOwner{0,17,290},SharedTextureOwner{0x802,17,290},SharedTextureOwner{0x801,18,290},SharedTextureOwner{0x801,17,291},SharedTextureOwner{0x801,0,290},SharedTextureOwner{0x801,17,0}}){
        check(!submittedSharedResource(&native,&held,bad,key,shape),"changed opened handle/generation/PID refused");
        check(!submittedSharedResource(&native,&held,key,bad,shape),"changed submitted handle/generation/PID refused");
    }
    for(auto bad:{SharedTextureShape{31,16,shape.format,1},SharedTextureShape{32,17,shape.format,1},SharedTextureShape{32,16,DXGI_FORMAT_B8G8R8A8_UNORM,1},SharedTextureShape{32,16,shape.format,2},SharedTextureShape{0,16,shape.format,1},SharedTextureShape{32,16,shape.format,1,2},SharedTextureShape{32,16,shape.format,1,0}})
        check(!submittedSharedResource(&native,&held,key,key,bad),"actual native and held shape must match submitted descriptor");
    auto rawShape=shape;rawShape.samples=9;check(!sharedTextureDescriptionMatches(native.desc,rawShape)&&!submittedSharedResource(&native,&held,key,key,rawShape),"raw packed word cannot substitute for decoded physical sample count");
    for(auto raw:{0u,4u,8u,9u}){SharedTextureShape decoded{};check(decodePinnedSwapShape(32,16,shape.format,raw,decoded),"native default/packed/MSAA metadata decodes");check(submittedSharedResource(&native,&held,key,key,decoded)==(raw!=4),"only physical single-sample metadata admits actual single-sample shared resource");}
    const auto original=native.desc;
    for(unsigned kind=0;kind<3;++kind){if(kind==0)native.desc.ArraySize=2;if(kind==1)native.desc.MipLevels=2;if(kind==2)native.desc.SampleDesc.Count=4;
        check(!submittedSharedResource(&native,&held,key,key,shape),"native array/mip/MSAA variants refused");native.desc=original;}
    held.desc.Width=31;check(!submittedSharedResource(&native,&held,key,key,shape),"held shape checked independently");held.desc.Width=32;
    native.sharing=E_FAIL;check(!submittedSharedResource(&native,&held,key,key,shape),"native failed/nonshared resource refused");native.sharing=S_OK;
    held.sharing=E_FAIL;check(!submittedSharedResource(&native,&held,key,key,shape),"held failed/nonshared resource refused");held.sharing=S_OK;
    native.dxgi=false;check(!submittedSharedResource(&native,&held,key,key,shape),"missing native DXGI interface refused");native.dxgi=true;
    native.texture=false;check(!submittedSharedResource(&native,&held,key,key,shape),"non-texture resource refused");native.texture=true;
    check(!submittedSharedResource(nullptr,&held,key,key,shape)&&!submittedSharedResource(&native,nullptr,key,key,shape),"null resources refused");
    check(native.references==1&&held.references==1&&foreign.references==1,"temporary public COM references balanced on all paths");
    std::printf("SIMULATED public COM metadata controls: %u checks PASS; not GPU or Steam evidence\n",checks);
}
bool unsupported(HRESULT result){return result==E_NOTIMPL||result==E_INVALIDARG||result==DXGI_ERROR_UNSUPPORTED;}
int gpuAttempt(D3D_DRIVER_TYPE type){
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    auto result=D3D11CreateDevice(nullptr,type,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,context.out());
    if(FAILED(result)){std::printf("D3D11 sharing unavailable type=%u stage=device hr=%08lx\n",unsigned(type),static_cast<unsigned long>(result));return 77;}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=32;desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
    Com<ID3D11Texture2D> creator;
    result=device->CreateTexture2D(&desc,nullptr,creator.out());
    if(FAILED(result)&&unsupported(result)){std::printf("D3D11 sharing unavailable type=%u stage=shared-create hr=%08lx\n",unsigned(type),static_cast<unsigned long>(result));return 77;}
    check(SUCCEEDED(result),"actual shared RGBA texture creation");
    Com<IDXGIResource> shared;check(SUCCEEDED(creator->QueryInterface(IID_IDXGIResource,reinterpret_cast<void**>(shared.out()))),"actual DXGI resource");HANDLE handle=nullptr;
    result=shared->GetSharedHandle(&handle);
    if(FAILED(result)&&unsupported(result)){std::printf("D3D11 sharing unavailable type=%u stage=get-handle hr=%08lx\n",unsigned(type),static_cast<unsigned long>(result));return 77;}
    check(SUCCEEDED(result)&&handle,"actual legacy shared handle");
    Com<ID3D11Texture2D> native,held,foreign;
    result=device->OpenSharedResource(handle,IID_ID3D11Texture2D,reinterpret_cast<void**>(native.out()));
    if(FAILED(result)&&unsupported(result)){std::printf("D3D11 sharing unavailable type=%u stage=open hr=%08lx\n",unsigned(type),static_cast<unsigned long>(result));return 77;}
    check(SUCCEEDED(result),"actual separately opened native wrapper");
    check(SUCCEEDED(device->OpenSharedResource(handle,IID_ID3D11Texture2D,reinterpret_cast<void**>(held.out()))),"actual separately opened held wrapper");
    Com<IUnknown> nativeId,heldId;check(SUCCEEDED(native->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(nativeId.out())))&&SUCCEEDED(held->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(heldId.out()))),"actual COM identities");
    check(nativeId.p!=heldId.p,"actual separate opens have different IUnknown identities");
    Com<ID3D11ShaderResourceView> srv;check(SUCCEEDED(device->CreateShaderResourceView(native.p,nullptr,srv.out())),"actual source SRV");
    context->PSSetShaderResources(0,1,&srv.p);Com<ID3D11ShaderResourceView> bound;context->PSGetShaderResources(0,1,bound.out());Com<ID3D11Resource> actual;bound->GetResource(actual.out());
    const SharedTextureOwner key{static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle)),17,290};SharedTextureShape shape{};
    check(decodePinnedSwapShape(32,16,DXGI_FORMAT_R8G8B8A8_UNORM,9,shape)&&shape.samples==1&&shape.arraySize==1,"GPU fixture consumes final normalized descriptor 9, not fabricated sample metadata");
    D3D11_TEXTURE2D_DESC actualDescription{};held->GetDesc(&actualDescription);check(sharedTextureDescriptionMatches(actualDescription,shape),"production cold-import guard consumes descriptor 9 and real opened kernel texture description");
    check(submittedSharedResource(actual.p,held.p,key,key,shape),"production admission consumes descriptor 9 with actual bound-SRV allocation and separately-opened hold");
    shared.reset();creator.reset();check(submittedSharedResource(actual.p,held.p,key,key,shape),"held/native opens retain allocation after creator wrapper release");
    check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,foreign.out())),"actual foreign shared texture with same dimensions");
    check(!submittedSharedResource(foreign.p,held.p,key,key,shape),"actual same-size foreign allocation refused");
    check(!submittedSharedResource(actual.p,held.p,{key.handle,key.generation+1,key.processId},key,shape),"actual resource still refused for stale generation");
    check(!submittedSharedResource(actual.p,held.p,{key.handle,key.generation,key.processId+1},key,shape),"actual resource still refused for different process");
    ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(0,1,&none);std::printf("ACTUAL D3D11 sharing type=%u: %u checks PASS; not Steam/video evidence\n",unsigned(type),checks);return 0;
}
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Choose --metadata or --gpu explicitly");
    if(!std::strcmp(argv[1],"--metadata")){metadata();return 0;}
    if(std::strcmp(argv[1],"--gpu"))throw std::runtime_error("Unknown fixture mode");
    int result=gpuAttempt(D3D_DRIVER_TYPE_WARP);return result==77?gpuAttempt(D3D_DRIVER_TYPE_HARDWARE):result;
}catch(const std::exception& error){std::fprintf(stderr,"Shared-resource fixture failed after %u checks: %s\n",checks,error.what());return 1;}}
