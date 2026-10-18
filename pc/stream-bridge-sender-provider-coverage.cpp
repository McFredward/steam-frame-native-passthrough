#include "stream-bridge-sender-provider-coverage.h"
#include "stream-bridge-sender-provider-coverage-render.h"
#include <d3d11_4.h>
#include <cstring>
#include <cmath>
#include <limits>

namespace stream_bridge::sender {
namespace {
template<class T> void release(T*& p){if(p){p->Release();p=nullptr;}}
template<class To,class From> To bits(From from){static_assert(sizeof(To)==sizeof(From),"Win64 function pointers");To to{};std::memcpy(&to,&from,sizeof to);return to;}
IUnknown* identity(IUnknown* resource){IUnknown* value=nullptr;if(resource)resource->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&value));return value;}
bool same(IUnknown* a,IUnknown* b){auto x=identity(a),y=identity(b);const bool yes=x&&x==y;release(x);release(y);return yes;}
using Map=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using Unmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
constexpr unsigned StageCount=4,MaxOpened=64*3;
struct MultithreadScope{ID3D11Multithread* value;explicit MultithreadScope(ID3D11Multithread* p):value(p){if(value)value->Enter();}~MultithreadScope(){if(value)value->Leave();}};
}
struct SenderCoverage::Impl {
    DirectModeBoundary& direct;NvencBoundary& boundary;ControlRuntime& control;
    mutable std::mutex mutex;std::atomic<bool> enabled{false},prepared{false};std::atomic<unsigned> state{0};std::atomic<std::uint32_t> lastPid{0},preparedPid{0};
    std::atomic<std::uint64_t> failedSerial{0};std::atomic<bool> retirePending{false};std::uintptr_t module=0;bool stopped=false;
    ID3D11Device* pendingDevice=nullptr;ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
    ID3D11Multithread* multithread=nullptr;void** table=nullptr;NativeDraw originalDraw=nullptr;Map originalMap=nullptr;Unmap originalUnmap=nullptr;Copy originalCopy=nullptr;
    std::array<ID3D11Texture2D*,2> witnessTargets{};
    ID3D11Texture2D* target=nullptr;ID3D11RenderTargetView* targetView=nullptr;ID3D11BlendState* blend=nullptr;
    std::uint32_t width=0,height=0;std::uint64_t activeSerial=0,activeEpoch=0;unsigned drawBits=0;std::array<unsigned,2> witnessBits{};bool invalid=false;
    IUnknown* nativeOutput=nullptr;std::uint32_t activePid=0;
    struct Opened {vr::SharedTextureHandle_t handle=0;std::uint64_t generation=0;std::uint32_t pid=0;ID3D11Texture2D* texture=nullptr;};
    std::array<Opened,MaxOpened> opened{};std::uint64_t inventoryEpoch=0;
    struct Constant {ID3D11Resource* resource=nullptr;void* pointer=nullptr;std::array<std::uint8_t,128> bytes{};std::uint64_t present=0;bool valid=false;};
    std::array<Constant,4> constants{};
    struct Stage {std::array<ID3D11Texture2D*,2> witnesses{};std::array<unsigned,2> witnessBits{};ID3D11Texture2D* texture=nullptr;IUnknown* input=nullptr;std::uint64_t present=0,submission=0,encoderGeneration=0;void* encoder=nullptr;std::uint32_t pid=0;};
    std::array<Stage,StageCount> stages{};
    static std::atomic<Impl*> instance;
    Impl(DirectModeBoundary& d,NvencBoundary& b,ControlRuntime& c):direct(d),boundary(b),control(c){}
    bool site(void* address,std::uintptr_t rva)const{return module&&reinterpret_cast<std::uintptr_t>(address)==module+rva;}
    static void deviceOpened(void*,std::uint64_t,void* value,NV_ENC_DEVICE_TYPE type,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);if(type!=NV_ENC_DEVICE_TYPE_DIRECTX||!value){s.state=1;return;}
        ID3D11Device* actual=nullptr;
        if(FAILED(static_cast<IUnknown*>(value)->QueryInterface(IID_ID3D11Device,reinterpret_cast<void**>(&actual))))return;
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock||s.stopped||(s.pendingDevice&&!same(s.pendingDevice,actual))||(s.device&&!same(s.device,actual))){release(actual);return;}
        if(!s.pendingDevice){s.pendingDevice=actual;s.state=2;}else release(actual);
    }
    static void inputCapture(const NvencOutputEvidence& e,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);
        if(!lock||!s.enabled||!e.exactInput||e.width!=s.width||e.height!=s.height||e.subresource)return;
        // The exact CopyResource receipt is consumed once, before Encode. It is
        // subsequently addressed only by accepted NVENC submission identity.
        Stage* found=nullptr;for(auto& slot:s.stages)if(slot.present&&!slot.submission&&same(slot.input,static_cast<IUnknown*>(e.resource))){if(found)return;found=&slot;}
        if(found){found->submission=e.submission;found->encoderGeneration=e.encoderGeneration;found->encoder=e.encoder;}
    }
    static void inputRetire(const NvencOutputEvidence& e,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock){s.retirePending=true;return;}
        for(auto& slot:s.stages)if(slot.submission==e.submission&&slot.encoderGeneration==e.encoderGeneration&&slot.encoder==e.encoder){release(slot.input);slot.present=slot.submission=slot.encoderGeneration=0;slot.encoder=nullptr;}
    }
    static bool augment(Bytes candidate,const NvencOutputEvidence& e,std::vector<std::uint8_t>& output,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);if(!e.exactInput||!e.submission||e.width>8192||e.height>8192)return false;
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock||!s.enabled)return false;
        std::vector<std::uint8_t> pixels;std::uint32_t maskPid=0;
        FrameDescriptor descriptor{e.submission,e.encoderGeneration,static_cast<std::uint16_t>(e.width),static_cast<std::uint16_t>(e.height)};
        for(auto& slot:s.stages)if(slot.submission==e.submission&&slot.encoderGeneration==e.encoderGeneration&&slot.encoder==e.encoder&&slot.texture&&slot.present){
            D3D11_MAPPED_SUBRESOURCE data{};
            // The legacy atlas writes alpha1 even in its background. Only an
            // actual submitted SOURCE interior texel below1 admits raw RGBA.
            // Encoded-frame padding and native sampling outside bounds cannot
            // serve as this witness. The tiny copies share this exact receipt.
            bool rawWitness=(slot.witnessBits[0]|slot.witnessBits[1])!=0;
            for(unsigned kind=0;kind<2;++kind)if(slot.witnessBits[kind]){
                D3D11_MAPPED_SUBRESOURCE sample{};
                if(SUCCEEDED(s.originalMap(s.context,slot.witnesses[kind],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&sample))){
                    rawWitness=rawWitness&&sourceInteriorTranslucent(sample,slot.witnessBits[kind]);
                    s.originalUnmap(s.context,slot.witnesses[kind],0);
                }else rawWitness=false;
            }
            if(!rawWitness)break;
            pixels.resize(std::size_t(s.width)*s.height);
            const auto result=s.originalMap(s.context,slot.texture,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&data);
            if(SUCCEEDED(result)){
                bool valid=data.pData&&data.RowPitch>=s.width;
                if(valid)for(unsigned y=0;y<s.height;++y){auto* row=static_cast<const std::uint8_t*>(data.pData)+std::size_t(y)*data.RowPitch;auto* out=pixels.data()+std::size_t(y)*s.width;for(unsigned x=0;x<s.width;++x)out[x]=std::uint8_t(255-row[x]);}
                s.originalUnmap(s.context,slot.texture,0);
                if(valid)maskPid=slot.pid;else pixels.clear();
            }else pixels.clear();
            break; // Never block or substitute a later staging frame.
        }
        // Do not hold the GPU metadata mutex while scanning/compressing the
        // CPU copy or authenticating it. Native Draw must never wait for RLE.
        lock.unlock();
        if(!pixels.empty()){std::vector<std::uint8_t> rle;if(encodeCoverageRle({pixels.data(),pixels.size()},rle)&&s.control.certifyAccessUnit(candidate,descriptor,CoverageEncoding::Rle,{rle.data(),rle.size()},output)){s.lastPid=maskPid;s.state=7;return true;}}
        // Discovery/coded-boundary certificate without a mask is useful and
        // explicitly does NOT permit transparent rendering on the receiver.
        return s.control.certifyAccessUnit(candidate,descriptor,CoverageEncoding::None,{},output);
    }
    bool installContext(){
        if(!device||device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)return false;
        device->GetImmediateContext(&context);if(!context||FAILED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(&multithread))))return false;
        // Public synchronization is enabled cold before admitting any companion
        // work. It remains enabled for this device's process lifetime.
        if(!multithread->GetMultithreadProtected())multithread->SetMultithreadProtected(TRUE);
        if(!multithread->GetMultithreadProtected())return false;
        table=*reinterpret_cast<void***>(context);originalDraw=bits<NativeDraw>(table[13]);originalMap=bits<Map>(table[14]);originalUnmap=bits<Unmap>(table[15]);originalCopy=bits<Copy>(table[47]);
        if(!originalDraw||!originalMap||!originalUnmap||!originalCopy)return false;
        Impl* expected=nullptr;if(!instance.compare_exchange_strong(expected,this)&&expected!=this)return false;
        std::array<unsigned,4> positions{{13,14,15,47}};std::array<void*,4> before{{bits<void*>(originalDraw),bits<void*>(originalMap),bits<void*>(originalUnmap),bits<void*>(originalCopy)}};
        std::array<void*,4> after{{bits<void*>(&draw),bits<void*>(&map),bits<void*>(&unmap),bits<void*>(&copy)}};
        DWORD old=0;if(!VirtualProtect(table,48*sizeof(void*),PAGE_READWRITE,&old))return false;
        unsigned count=0;for(;count<4;++count)if(InterlockedCompareExchangePointer(table+positions[count],after[count],before[count])!=before[count])break;
        DWORD ignored=0;bool protectedAgain=VirtualProtect(table,48*sizeof(void*),old,&ignored)!=0;
        if(count!=4||!protectedAgain){DWORD rollback=0;if(VirtualProtect(table,48*sizeof(void*),PAGE_READWRITE,&rollback)){for(unsigned i=0;i<count;++i)InterlockedCompareExchangePointer(table+positions[i],before[i],after[i]);VirtualProtect(table,48*sizeof(void*),old,&ignored);}return false;}
        enabled=true;return true;
    }
    bool allocate(std::uint32_t w,std::uint32_t h){
        if(!w||!h||w>8192||h>8192||std::size_t(w)*h>MaximumCoverageBytes)return false;
        // No in-flight readback is reused at another size. Retiring submitted
        // generations happens through the NVENC owner before resize admission.
        for(const auto& slot:stages)if(slot.submission)return false;
        prepared=false;preparedPid=0;state=4;release(targetView);release(target);for(auto& slot:stages){release(slot.texture);release(slot.input);for(auto& witness:slot.witnesses)release(witness);slot={};}
        for(auto& witness:witnessTargets)release(witness);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&target))||FAILED(device->CreateRenderTargetView(target,nullptr,&targetView)))return false;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for(auto& slot:stages)if(FAILED(device->CreateTexture2D(&desc,nullptr,&slot.texture)))return false;
        for(unsigned kind=0;kind<2;++kind){
            desc.Width=5;desc.Height=40;desc.Format=kind?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
            for(auto& slot:stages)if(FAILED(device->CreateTexture2D(&desc,nullptr,&slot.witnesses[kind])))return false;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&witnessTargets[kind])))return false;
            desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        }
        if(!blend&&!createTransmittanceBlend(device,&blend))return false;
        width=w;height=h;activeSerial=0;release(nativeOutput);prepared=true;return true;
    }
    void refresh(const DirectInventory& inventory){
        if(!inventory.valid){prepared=false;preparedPid=0;return;}
        if(inventory.epoch==inventoryEpoch)return;
        prepared=false;preparedPid=0;state=4;
        for(auto& old:opened){if(!old.handle)continue;bool still=false;for(const auto& set:inventory.swaps)if(set.generation==old.generation)for(auto h:set.handles)if(h==old.handle)still=true;if(!still){release(old.texture);old={};}}
        for(const auto& set:inventory.swaps)if(set.generation)for(auto handle:set.handles){bool exists=false;for(const auto& old:opened)exists|=old.handle==handle&&old.generation==set.generation;if(exists)continue;
            ID3D11Texture2D* texture=nullptr;
            if(FAILED(device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)),IID_ID3D11Texture2D,reinterpret_cast<void**>(&texture))))continue;
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            if(desc.Width!=set.description.nWidth||desc.Height!=set.description.nHeight||desc.Format!=static_cast<DXGI_FORMAT>(set.description.nFormat)||desc.SampleDesc.Count!=set.description.nSampleCount){release(texture);continue;}
            for(auto& slot:opened)if(!slot.handle){slot={handle,set.generation,set.processId,texture};texture=nullptr;break;}
            release(texture);
        }
        inventoryEpoch=inventory.epoch;prepared=target&&targetView&&blend;
    }
    bool luma(ID3D11DeviceContext* ctx,UINT count,UINT start,const DirectFrame& frame){
        if(!prepared||count!=4||start||frame.registryEpoch!=inventoryEpoch||frame.presentSerial==failedSerial)return false;
        if(activeSerial!=frame.presentSerial){activeSerial=frame.presentSerial;activeEpoch=frame.registryEpoch;drawBits=0;witnessBits={};invalid=false;activePid=0;release(nativeOutput);const float one[4]{1,1,1,1};ctx->ClearRenderTargetView(targetView,one);}
        if(invalid||activeEpoch!=frame.registryEpoch)return false;
        ID3D11Buffer* buffer=nullptr;ctx->PSGetConstantBuffers(2,1,&buffer);Constant* cb=nullptr;for(auto& entry:constants)if(entry.valid&&entry.present==frame.presentSerial&&same(entry.resource,buffer))cb=&entry;release(buffer);if(!cb)return false;
        float eyeValue=0;std::memcpy(&eyeValue,cb->bytes.data()+76,sizeof eyeValue);if(!std::isfinite(eyeValue)||(eyeValue!=0&&eyeValue!=1))return false;const unsigned eye=static_cast<unsigned>(eyeValue);
        ID3D11ShaderResourceView* view=nullptr;ctx->PSGetShaderResources(0,1,&view);ID3D11Resource* resource=nullptr;D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        if(view){view->GetResource(&resource);view->GetDesc(&viewDesc);}release(view);if(!resource)return false;
        ID3D11Texture2D* sourceTexture=nullptr;resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&sourceTexture));D3D11_TEXTURE2D_DESC sourceDesc{};if(sourceTexture)sourceTexture->GetDesc(&sourceDesc);release(sourceTexture);
        const bool rgba=viewDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||viewDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra=viewDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||viewDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if((!rgba&&!bgra)||sourceDesc.ArraySize!=1||sourceDesc.MipLevels!=1||sourceDesc.SampleDesc.Count!=1||viewDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||viewDesc.Texture2D.MostDetailedMip){release(resource);return false;}
        unsigned sourceBit=0,sourceIndex=0;std::uint32_t pid=0;vr::VRTextureBounds_t bounds{};
        for(unsigned layer=0;layer<frame.layerCount;++layer){const auto& requested=frame.layers[layer][eye];for(const auto& known:opened)if(known.handle==requested.submitted.hTexture&&known.generation==requested.swapGeneration&&same(known.texture,resource)){if(sourceBit){release(resource);return false;}sourceIndex=2*layer+eye;sourceBit=1u<<sourceIndex;pid=requested.processId;bounds=requested.submitted.bounds;}}
        if(!sourceBit||(drawBits&sourceBit)||(activePid&&activePid!=pid)){release(resource);return false;}
        const unsigned kind=bgra?1:0;
        ID3D11Texture2D* sourceForCopy=nullptr;resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&sourceForCopy));
        const bool copied=copySubmittedInterior(ctx,sourceForCopy,bounds.uMin,bounds.vMin,bounds.uMax,bounds.vMax,witnessTargets[kind],sourceIndex);
        release(sourceForCopy);release(resource);if(!copied)return false;witnessBits[kind]|=sourceBit;

        ID3D11RenderTargetView* out=nullptr;ID3D11DepthStencilView* depth=nullptr;ctx->OMGetRenderTargets(1,&out,&depth);if(depth){release(depth);release(out);return false;}
        ID3D11Resource* outputResource=nullptr;if(out)out->GetResource(&outputResource);release(out);
        ID3D11Texture2D* outputTexture=nullptr;if(outputResource)outputResource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&outputTexture));D3D11_TEXTURE2D_DESC desc{};if(outputTexture)outputTexture->GetDesc(&desc);release(outputTexture);
        bool okay=outputResource&&desc.Width==width&&desc.Height==height&&(desc.Format==DXGI_FORMAT_NV12||desc.Format==DXGI_FORMAT_P010)&&(!nativeOutput||same(nativeOutput,outputResource));
        if(okay&&!nativeOutput)nativeOutput=identity(outputResource);
        release(outputResource);if(!okay)return false;
        if(!drawTransmittance(ctx,targetView,blend,originalDraw,count,start))return false;
        drawBits|=sourceBit;activePid=pid;if(drawBits==((1u<<(2*frame.layerCount))-1)){preparedPid=pid;if(state!=7)state=6;}return true;
    }
    static void STDMETHODCALLTYPE draw(ID3D11DeviceContext* ctx,UINT count,UINT start){
        auto& s=*instance.load();const auto* frame=DirectModeBoundary::currentFrame();const bool ours=ctx==s.context&&s.enabled&&s.site(__builtin_return_address(0),0x9f3865)&&frame;
        if(!ours){s.originalDraw(ctx,count,start);return;}
        MultithreadScope protect(s.multithread);s.originalDraw(ctx,count,start);
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock){s.failedSerial=frame->presentSerial;return;}
        if(!s.luma(ctx,count,start,*frame)){s.invalid=true;s.failedSerial=frame->presentSerial;}
    }
    static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* ctx,ID3D11Resource* resource,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* output){
        auto& s=*instance.load();const auto result=s.originalMap(ctx,resource,sub,type,flags,output);
        if(ctx!=s.context||!s.enabled||!s.site(__builtin_return_address(0),0x9f36f2)||FAILED(result)||!output||!output->pData||sub||type!=D3D11_MAP_WRITE_DISCARD||flags)return result;
        const auto* frame=DirectModeBoundary::currentFrame();if(!frame)return result;
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock){s.failedSerial=frame->presentSerial;return result;}
        ID3D11Buffer* buffer=nullptr;if(resource)resource->QueryInterface(IID_ID3D11Buffer,reinterpret_cast<void**>(&buffer));D3D11_BUFFER_DESC desc{};if(buffer)buffer->GetDesc(&desc);release(buffer);
        if(desc.ByteWidth!=128||!(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER))return result;
        Constant* found=nullptr;for(auto& c:s.constants)if(same(c.resource,resource))found=&c;for(auto& c:s.constants)if(!found&&!c.resource){c.resource=resource;resource->AddRef();found=&c;}
        if(found){found->valid=false;found->pointer=output->pData;found->present=frame->presentSerial;}return result;
    }
    static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* ctx,ID3D11Resource* resource,UINT sub){
        auto& s=*instance.load();if(ctx==s.context&&s.enabled&&s.site(__builtin_return_address(0),0x9f372c)&&!sub){std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(lock)for(auto& c:s.constants)if(c.pointer&&same(c.resource,resource)){std::memcpy(c.bytes.data(),c.pointer,c.bytes.size());c.pointer=nullptr;c.valid=true;}}
        s.originalUnmap(ctx,resource,sub);
    }
    static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* ctx,ID3D11Resource* destination,ID3D11Resource* source){
        auto& s=*instance.load();s.originalCopy(ctx,destination,source);const auto* frame=DirectModeBoundary::currentFrame();
        if(ctx!=s.context||!s.enabled||!s.site(__builtin_return_address(0),0xa217fb)||!frame)return;
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock)return;
        if(!s.prepared||s.invalid||frame->presentSerial!=s.activeSerial||frame->presentSerial==s.failedSerial||frame->registryEpoch!=s.activeEpoch||!same(source,s.nativeOutput)||s.drawBits!=((1u<<(2*frame->layerCount))-1))return;
        // Frozen slots cannot be evicted by a resource pointer reused later.
        Stage* free=nullptr;for(auto& slot:s.stages){if(!slot.submission&&same(slot.input,destination)){release(slot.input);slot.present=0;}if(!slot.present&&!free)free=&slot;}
        if(!free)return;
        MultithreadScope protect(s.multithread);s.originalCopy(ctx,free->texture,s.target);for(unsigned kind=0;kind<2;++kind)if(s.witnessBits[kind])s.originalCopy(ctx,free->witnesses[kind],s.witnessTargets[kind]);free->witnessBits=s.witnessBits;free->input=identity(destination);if(!free->input)return;free->present=frame->presentSerial;free->pid=s.activePid;
    }
};
std::atomic<SenderCoverage::Impl*> SenderCoverage::Impl::instance{nullptr};
SenderCoverage::SenderCoverage(DirectModeBoundary& d,NvencBoundary& b,ControlRuntime& c):impl_(new Impl(d,b,c)){
    b.setDeviceObserver({Impl::deviceOpened,impl_.get()});b.setInputObserver({Impl::inputCapture,Impl::inputRetire,impl_.get()});b.setOutputAugmenter({Impl::augment,impl_.get()});
}
SenderCoverage::~SenderCoverage(){stop();(void)impl_.release();} // callbacks/COM retained until process exit
bool SenderCoverage::configure(std::uintptr_t module)noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->module||!module)return false;impl_->module=module;return true;}
void SenderCoverage::tick()noexcept{
    auto& s=*impl_;const auto encoders=s.boundary.encoderStates();const auto inventory=s.direct.inventory();
    std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock||s.stopped||!s.module)return;
    try{
        if(!s.device&&s.pendingDevice){s.device=s.pendingDevice;s.pendingDevice=nullptr;if(!s.installContext()){s.stopped=true;s.state=3;return;}s.state=4;}
        if(!s.enabled)return;
        if(s.retirePending.exchange(false)){for(auto& slot:s.stages){release(slot.input);slot.present=slot.submission=slot.encoderGeneration=0;slot.encoder=nullptr;}}
        EncoderState actual{};unsigned count=0;for(const auto& e:encoders)if(e.encoder&&e.hevc){actual=e;++count;}
        if(count!=1){s.prepared=false;s.preparedPid=0;s.state=4;return;}
        if(s.width!=actual.width||s.height!=actual.height||!s.target){if(!s.allocate(actual.width,actual.height)){s.state=5;return;}}
        s.prepared=s.target&&s.targetView&&s.blend;for(const auto& slot:s.stages)s.prepared=s.prepared&&slot.texture&&slot.witnesses[0]&&slot.witnesses[1];
        s.refresh(inventory);
    }catch(...){s.prepared=false;s.state=5;}
}
void SenderCoverage::stop()noexcept{
    auto& s=*impl_;s.enabled=false;s.prepared=false;s.lastPid=0;s.preparedPid=0;s.state=8;
    std::lock_guard<std::mutex> lock(s.mutex);s.stopped=true;
    if(s.table){std::array<unsigned,4> index{{13,14,15,47}};std::array<void*,4> native{{bits<void*>(s.originalDraw),bits<void*>(s.originalMap),bits<void*>(s.originalUnmap),bits<void*>(s.originalCopy)}};std::array<void*,4> ours{{bits<void*>(&Impl::draw),bits<void*>(&Impl::map),bits<void*>(&Impl::unmap),bits<void*>(&Impl::copy)}};DWORD old=0;if(VirtualProtect(s.table,48*sizeof(void*),PAGE_READWRITE,&old)){for(unsigned i=0;i<4;++i)InterlockedCompareExchangePointer(s.table+index[i],native[i],ours[i]);DWORD ignored=0;VirtualProtect(s.table,48*sizeof(void*),old,&ignored);}}
    // No COM or staging release while a cached callback/borrowed AU can exist.
}
bool SenderCoverage::ready(std::uint32_t pid)const noexcept{return impl_->enabled&&impl_->prepared&&impl_->preparedPid==pid&&impl_->direct.ownsProcess(pid);}
bool SenderCoverage::operational(std::uint32_t pid)const noexcept{return ready(pid)&&impl_->lastPid==pid;}
const char* SenderCoverage::stateName()const noexcept{
    switch(impl_->state.load()){case 0:return "coverage-waiting-native-device";case 1:return "coverage-unsupported-device";case 2:return "coverage-device-observed";case 3:return "coverage-context-or-hook-refused";case 4:return "coverage-waiting-supported-encoder-or-source";case 5:return "coverage-pool-preparation-refused";case 6:return "coverage-source-pair-prepared-waiting-raw-receipt";case 7:return "coverage-native-receipt-certified-not-selected-proof";default:return "coverage-stopped";}
}
} // namespace stream_bridge::sender
