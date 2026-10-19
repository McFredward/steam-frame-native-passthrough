#include "stream-bridge-sender-provider-coverage.h"
#include "stream-bridge-sender-provider-coverage-render.h"
#include "stream-bridge-sender-provider-coverage-stages.h"
#include "stream-bridge-source-contract.h"
#include <d3d11_4.h>
#include <cstring>
#include <cmath>
#include <limits>
#include <chrono>

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
std::uint64_t clockMicros()noexcept{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
std::uint64_t elapsedMicros(std::uint64_t start,std::uint64_t end)noexcept{return start&&end>=start?end-start:0;}
struct MultithreadScope{ID3D11Multithread* value;explicit MultithreadScope(ID3D11Multithread* p):value(p){if(value)value->Enter();}~MultithreadScope(){if(value)value->Leave();}};
}
struct CoverageContextFacade::Impl {
    ID3D11DeviceContext* context=nullptr;ID3D11Multithread* multithread=nullptr;
    void** original=nullptr;void** facade=nullptr;void* allocation=nullptr;
    std::array<void*,149> nativeInitial{},expected{};unsigned count=0;
    bool prepared=false,everPublished=false;
    static bool read(const void* address,void* value,std::size_t length)noexcept{
        SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),address,value,length,&got)&&got==length;
    }
    bool owns()const noexcept{
        if(!prepared)return false;
        void** current=nullptr;std::array<void*,149> observed{};
        return read(context,&current,sizeof current)&&current==facade&&read(facade,observed.data(),count*sizeof(void*))&&
            std::memcmp(observed.data(),expected.data(),count*sizeof(void*))==0;
    }
};
CoverageContextFacade::CoverageContextFacade():impl_(new Impl){}
CoverageContextFacade::~CoverageContextFacade(){
    restore();if(impl_->everPublished){(void)impl_.release();return;}
    if(impl_->allocation)VirtualFree(impl_->allocation,0,MEM_RELEASE);
    release(impl_->multithread);release(impl_->context);
}
bool CoverageContextFacade::prepare(ID3D11DeviceContext* context,const std::array<std::uintptr_t,4>& callbacks)noexcept{
    auto& s=*impl_;if(s.prepared||s.context||!context||(reinterpret_cast<std::uintptr_t>(context)&7u))return false;
    for(auto callback:callbacks)if(!callback)return false;
    // The exact public ABI size is selected by the highest supported same-
    // pointer interface. An alternate COM alias never licenses a longer table.
    const std::array<GUID,4> versions{{__uuidof(ID3D11DeviceContext4),__uuidof(ID3D11DeviceContext3),__uuidof(ID3D11DeviceContext2),__uuidof(ID3D11DeviceContext1)}};
    const unsigned lengths[4]{149,147,144,134};
    for(unsigned i=0;i<versions.size();++i){void* queried=nullptr;
        if(SUCCEEDED(context->QueryInterface(versions[i],&queried))){
            if(!queried)return false;
            if(queried!=context){static_cast<IUnknown*>(queried)->Release();return false;}
            s.context=static_cast<ID3D11DeviceContext*>(queried);s.count=lengths[i];break;
        }
    }
    if(!s.context){context->AddRef();s.context=context;s.count=115;}
    if(FAILED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(&s.multithread)))||
       !s.multithread->GetMultithreadProtected())return false;
    MultithreadScope protect(s.multithread);
    if(!Impl::read(context,&s.original,sizeof s.original)||!s.original||
       !Impl::read(s.original,s.nativeInitial.data(),s.count*sizeof(void*)))return false;
    for(unsigned i=0;i<s.count;++i)if(!s.nativeInitial[i])return false;
    SYSTEM_INFO info{};GetSystemInfo(&info);const auto page=std::size_t(info.dwPageSize);
    if(page<s.count*16||page<s.count*sizeof(void*))return false;
    s.allocation=VirtualAlloc(nullptr,2*page,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!s.allocation)return false;
    s.facade=static_cast<void**>(s.allocation);auto* code=static_cast<std::uint8_t*>(s.allocation)+page;
    for(unsigned i=0;i<s.count;++i){auto* thunk=code+16*i;
        // ENDBR64 is an IBT landing pad and a no-op where CET is disabled.
        // movabs RAX=&original[slot]; jmp [RAX] preserves all arguments,
        // nonvolatile registers, stack/shadow-space and flags, with no frame.
        const std::uint8_t opcodes[16]{0xf3,0x0f,0x1e,0xfa,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0x20};
        std::memcpy(thunk,opcodes,sizeof opcodes);const auto source=reinterpret_cast<std::uintptr_t>(s.original+i);
        std::memcpy(thunk+6,&source,sizeof source);s.expected[i]=thunk;
    }
    const unsigned slots[4]{13,14,15,47};for(unsigned i=0;i<4;++i)s.expected[slots[i]]=reinterpret_cast<void*>(callbacks[i]);
    std::memcpy(s.facade,s.expected.data(),s.count*sizeof(void*));
    DWORD old=0;if(!VirtualProtect(code,page,PAGE_EXECUTE_READ,&old)||!FlushInstructionCache(GetCurrentProcess(),code,page))return false;
    // Only the generated forwarding instructions become RX. The object-local
    // facade table stays writable; runtime changes are observed, never faulted.
    s.prepared=true;return true;
}
bool CoverageContextFacade::publish()noexcept{
    auto& s=*impl_;if(!s.prepared||!s.multithread->GetMultithreadProtected())return false;
    MultithreadScope protect(s.multithread);if(s.owns())return true;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(s.context,&info,sizeof info)||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
    const auto access=info.Protect&0xffu;
    if(access!=PAGE_READWRITE&&access!=PAGE_WRITECOPY&&access!=PAGE_EXECUTE_READWRITE&&access!=PAGE_EXECUTE_WRITECOPY)return false;
    const auto previous=InterlockedCompareExchangePointer(reinterpret_cast<void**>(s.context),s.facade,s.original);
    if(previous!=s.original)return false;
    s.everPublished=true;return s.owns();
}
bool CoverageContextFacade::restore()noexcept{
    auto& s=*impl_;if(!s.prepared||!s.everPublished)return true;
    MultithreadScope protect(s.multithread);void** current=nullptr;
    if(!Impl::read(s.context,&current,sizeof current))return false;
    if(current==s.original)return true;
    // A later vptr or entry replacement has its own owner. Leave that entire
    // facade and all forwarding code retained rather than erase its hook.
    if(!s.owns())return false;
    return InterlockedCompareExchangePointer(reinterpret_cast<void**>(s.context),s.original,s.facade)==s.facade;
}
bool CoverageContextFacade::installed()const noexcept{return impl_->owns();}
unsigned CoverageContextFacade::slotCount()const noexcept{return impl_->count;}
std::uintptr_t CoverageContextFacade::originalFunction(unsigned slot)const noexcept{
    const auto& s=*impl_;if(!s.prepared||slot>=s.count)return 0;
    return reinterpret_cast<const volatile std::uintptr_t*>(s.original)[slot];
}
std::uint32_t CoverageContextFacade::nativeMutationCount()const noexcept{
    const auto& s=*impl_;if(!s.prepared)return 0;std::array<void*,149> observed{};
    if(!Impl::read(s.original,observed.data(),s.count*sizeof(void*)))return 0;
    std::uint32_t count=0;for(unsigned i=0;i<s.count;++i)if(observed[i]!=s.nativeInitial[i])++count;return count;
}

struct SenderCoverage::Impl {
    DirectModeBoundary& direct;NvencBoundary& boundary;ControlRuntime& control;CoverageGateCounters gates;
    mutable std::mutex mutex;std::atomic<bool> enabled{false},prepared{false};std::atomic<unsigned> state{0};std::atomic<std::uint32_t> lastPid{0},preparedPid{0};
    std::atomic<std::uint64_t> failedSerial{0};std::uintptr_t module=0;std::atomic<bool> stopped{false};
    ID3D11Device* pendingDevice=nullptr;ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
    void* nativeDevicePointer=nullptr; // Comparison only; never dereferenced as a guessed interface.
    ID3D11Multithread* multithread=nullptr;CoverageContextFacade facade;
    SourceContract sourceContract;
    ID3D11Texture2D* target=nullptr;ID3D11RenderTargetView* targetView=nullptr;ID3D11BlendState* blend=nullptr;
    std::uint32_t width=0,height=0;std::uint64_t activeSerial=0,activeEpoch=0;unsigned drawBits=0;std::uint64_t activeContract=0;bool invalid=false,poolReady=false;
    IUnknown* nativeOutput=nullptr;std::uint32_t activePid=0;
    struct Opened {vr::SharedTextureHandle_t handle=0;std::uint64_t generation=0;std::uint32_t pid=0;ID3D11Texture2D* texture=nullptr;};
    std::array<Opened,MaxOpened> opened{};std::uint64_t inventoryEpoch=0;
    struct Constant {ID3D11Resource* resource=nullptr;void* pointer=nullptr;std::array<std::uint8_t,128> bytes{};std::uint64_t present=0;bool valid=false;};
    std::array<Constant,4> constants{};
    struct StageResources {
        ID3D11Texture2D* texture=nullptr;ID3D11Query* completion=nullptr;ID3D11DeviceContext* context=nullptr;
        IUnknown* input=nullptr;
        bool ready()const noexcept{return texture&&completion&&context;}
    };
    using Stages=CoverageStages<StageResources,StageCount>;
    Stages stages;
    static std::atomic<Impl*> instance;
    Impl(DirectModeBoundary& d,NvencBoundary& b,ControlRuntime& c):direct(d),boundary(b),control(c){}
    bool site(void* address,std::uintptr_t rva)const{return module&&reinterpret_cast<std::uintptr_t>(address)==module+rva;}
    static void deviceOpened(void*,std::uint64_t,void* value,NV_ENC_DEVICE_TYPE type,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);s.gates.signal(CoverageSignal::DeviceOpened);if(type!=NV_ENC_DEVICE_TYPE_DIRECTX||!value){s.gates.refuse(CoverageGate::Device);s.state=1;return;}
        ID3D11Device* actual=nullptr;
        if(FAILED(static_cast<IUnknown*>(value)->QueryInterface(IID_ID3D11Device,reinterpret_cast<void**>(&actual)))){s.gates.refuse(CoverageGate::Device);return;}
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock||s.stopped||(s.pendingDevice&&!same(s.pendingDevice,actual))||(s.device&&!same(s.device,actual))){release(actual);return;}
        if(!s.pendingDevice){s.pendingDevice=actual;s.nativeDevicePointer=value;s.state=2;}else release(actual);
    }
    static void inputCapture(const NvencOutputEvidence& e,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);
        if(!s.enabled||!e.exactInput||e.subresource)return;
        // The exact CopyResource receipt is consumed once, before Encode. It is
        // subsequently addressed only by accepted NVENC submission identity.
        auto* input=identity(static_cast<IUnknown*>(e.resource));
        const bool matched=input&&s.stages.bind(reinterpret_cast<std::uintptr_t>(input),e.width,e.height,e.submission,e.encoder,e.encoderGeneration,clockMicros());
        release(input);
        if(matched)s.gates.signal(CoverageSignal::MatchedInputs);
        else s.gates.refuse(CoverageGate::InputReceipt);
    }
    static void inputRetire(const NvencOutputEvidence& e,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);auto owned=s.stages.read(e.submission,e.encoder,e.encoderGeneration);
        if(owned){release(owned->resources.input);owned->receipt={};owned.publish(Stages::Phase::Retired);}
    }
    static bool augment(Bytes candidate,const NvencOutputEvidence& e,std::vector<std::uint8_t>& output,void* opaque){
        auto& s=*static_cast<Impl*>(opaque);if(!e.exactInput||!e.submission||e.width>8192||e.height>8192)return s.gates.refuse(CoverageGate::InputReceipt);
        FrameDescriptor descriptor{e.submission,e.encoderGeneration,static_cast<std::uint16_t>(e.width),static_cast<std::uint16_t>(e.height)};
        // Reconfigure can Encode/Lock before the cold worker replaces control.
        // The old paired control binding is not new-generation mask permission.
        // Certificate-only discovery must remain possible to bootstrap that
        // fresh handshake, without reading/compressing any coverage staging.
        if(!e.coverageSessionAuthorized){s.gates.refuse(CoverageGate::CoverageSession);const bool certified=s.control.certifyAccessUnit(candidate,descriptor,CoverageEncoding::None,{},output);if(!certified)s.gates.refuse(CoverageGate::FrameCertificate);return certified;}
        if(!s.enabled)return s.gates.refuse(CoverageGate::Prepared);
        std::vector<std::uint8_t> pixels;std::uint32_t maskPid=0;std::uint64_t maskContract=0;
        {
            // Reading excludes reuse/resize of exactly this stage. It does not
            // own the render/cold mutex while Map copies and inverts 5 MiB.
            auto owned=s.stages.read(e.submission,e.encoder,e.encoderGeneration);
            if(owned){
                const auto& slot=owned->receipt;const auto& gpu=owned->resources;
                // Format permission comes from the live kernel-validated app.
                // Fully opaque eyes are legitimate; sparse pixel samples cannot
                // establish the semantics of an entire submitted texture.
                if(!s.sourceContract.current(slot.contract,slot.pid))s.gates.refuse(CoverageGate::SourceRevoked);
                else if(gpu.ready()&&slot.width==e.width&&slot.height==e.height){
                    pixels.resize(std::size_t(slot.width)*slot.height);
                    const auto started=clockMicros();CoverageReadbackResult receipt;
                    const auto result=readCoverageNonblocking(gpu.context,gpu.texture,gpu.completion,slot.width,slot.height,pixels.data(),pixels.size(),forwardGetData,forwardMap,forwardUnmap,receipt);
                    s.gates.completion(static_cast<std::uint32_t>(receipt.query),elapsedMicros(slot.copyMicros,started));
                    if(receipt.mapAttempted)s.gates.readback(static_cast<std::uint32_t>(result),elapsedMicros(slot.copyMicros,started),
                        elapsedMicros(slot.submissionMicros,started),elapsedMicros(started,clockMicros()));
                    if(SUCCEEDED(result)){maskPid=slot.pid;maskContract=slot.contract;}
                    else{s.gates.refuse(receipt.mapAttempted?CoverageGate::Readback:(receipt.query==S_FALSE?CoverageGate::QueryPending:CoverageGate::QueryError));pixels.clear();}
                }else s.gates.refuse(CoverageGate::StageReceipt);
            }else s.gates.refuse(CoverageGate::StageReceipt);
        }
        // Do not hold the GPU metadata mutex while scanning/compressing the
        // CPU copy or authenticating it. Native Draw must never wait for RLE.
        if(!pixels.empty()){
            std::vector<std::uint8_t> rle;
            if(!encodeCoverageRle({pixels.data(),pixels.size()},rle))s.gates.refuse(CoverageGate::RleEncode);
            else if(!s.sourceContract.current(maskContract,maskPid))s.gates.refuse(CoverageGate::SourceRevoked);
            else if(!s.control.certifyAccessUnit(candidate,descriptor,CoverageEncoding::Rle,{rle.data(),rle.size()},output))s.gates.refuse(CoverageGate::FrameCertificate);
            else{
                // Local EOF can revoke the format during compression or MAC.
                // Only publish the still-current capture; no stale mask leaves
                // this callback even when its original GPU readback was valid.
                if(!s.sourceContract.current(maskContract,maskPid)){output.clear();s.gates.refuse(CoverageGate::SourceRevoked);return false;}
                s.lastPid=maskPid;s.state=7;s.gates.signal(CoverageSignal::CertifiedMasks);return true;
            }
        }
        // Discovery/coded-boundary certificate without a mask is useful and
        // explicitly does NOT permit transparent rendering on the receiver.
        const bool certified=s.control.certifyAccessUnit(candidate,descriptor,CoverageEncoding::None,{},output);if(!certified)s.gates.refuse(CoverageGate::FrameCertificate);return certified;
    }
    static void STDMETHODCALLTYPE forwardDraw(ID3D11DeviceContext* context,UINT count,UINT start){
        auto& s=*instance.load();bits<NativeDraw>(s.facade.originalFunction(13))(context,count,start);
    }
    static HRESULT STDMETHODCALLTYPE forwardMap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE* output){
        auto& s=*instance.load();return bits<Map>(s.facade.originalFunction(14))(context,resource,sub,mode,flags,output);
    }
    static void STDMETHODCALLTYPE forwardUnmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub){
        auto& s=*instance.load();bits<Unmap>(s.facade.originalFunction(15))(context,resource,sub);
    }
    static void STDMETHODCALLTYPE forwardCopy(ID3D11DeviceContext* context,ID3D11Resource* destination,ID3D11Resource* source){
        auto& s=*instance.load();bits<Copy>(s.facade.originalFunction(47))(context,destination,source);
    }
    static void STDMETHODCALLTYPE forwardEnd(ID3D11DeviceContext* ctx,ID3D11Asynchronous* query){ctx->End(query);}
    static void STDMETHODCALLTYPE forwardFlush(ID3D11DeviceContext* ctx){ctx->Flush();}
    static HRESULT STDMETHODCALLTYPE forwardGetData(ID3D11DeviceContext* ctx,ID3D11Asynchronous* query,void* data,UINT size,UINT flags){return ctx->GetData(query,data,size,flags);}
    bool installContext(){
        if(!device||device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)return false;
        device->GetImmediateContext(&context);if(!context||FAILED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(&multithread))))return false;
        // The pinned native caller enables protection before NVENC Open. Keep
        // that native mode; never force it for an unsupported caller/device.
        if(!multithread->GetMultithreadProtected())return false;
        Impl* expected=nullptr;if(!instance.compare_exchange_strong(expected,this)&&expected!=this)return false;
        const std::array<std::uintptr_t,4> callbacks{{bits<std::uintptr_t>(&draw),bits<std::uintptr_t>(&map),bits<std::uintptr_t>(&unmap),bits<std::uintptr_t>(&copy)}};
        if(!facade.prepare(context,callbacks)||!facade.publish())return false;
        enabled=true;gates.signal(CoverageSignal::ContextsReady);return true;
    }
    bool dispatchOwned()const noexcept{return context&&multithread&&multithread->GetMultithreadProtected()&&facade.installed();}
    void loseDispatch()noexcept{
        // Public runtime mode changes can replace dispatch legitimately. Never
        // reinstall over those new/native/third-party entries or keep granting
        // coverage from receipts captured before the changed context contract.
        enabled=false;prepared=false;lastPid=0;preparedPid=0;stopped=true;state=9;
        for(auto& constant:constants){constant.valid=false;constant.pointer=nullptr;}
        // A borrowed encoder callback can still own a Reading stage. Revoking
        // authority never frees or overwrites its resources/receipt.
        gates.refuse(CoverageGate::Context);
    }
    bool allocate(std::uint32_t w,std::uint32_t h){
        if(!w||!h||w>8192||h>8192||std::size_t(w)*h>MaximumCoverageBytes)return false;
        // No in-flight readback is reused at another size. Retiring submitted
        // generations happens through the NVENC owner before resize admission.
        Stages::PoolClaim all;if(!stages.maintenance(all))return false;
        poolReady=false;prepared=false;preparedPid=0;state=4;release(targetView);release(target);
        for(auto& slot:all){auto& gpu=slot->resources;release(gpu.texture);release(gpu.completion);release(gpu.input);gpu={};slot->receipt={};}
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&target))||FAILED(device->CreateRenderTargetView(target,nullptr,&targetView)))return false;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC query{};query.Query=D3D11_QUERY_EVENT;
        for(auto& slot:all){auto& gpu=slot->resources;gpu.context=context;if(FAILED(device->CreateTexture2D(&desc,nullptr,&gpu.texture))||FAILED(device->CreateQuery(&query,&gpu.completion)))return false;}
        if(!blend&&!createTransmittanceBlend(device,&blend))return false;
        width=w;height=h;activeSerial=0;release(nativeOutput);
        for(auto& slot:all)slot.publish(Stages::Phase::Free);
        poolReady=true;prepared=true;return true;
    }
    void refresh(const DirectInventory& inventory){
        if(!inventory.valid){gates.refuse(CoverageGate::Inventory);prepared=false;preparedPid=0;return;}
        if(inventory.epoch==inventoryEpoch)return;
        gates.signal(CoverageSignal::InventoryRefreshes);
        prepared=false;preparedPid=0;state=4;
        for(auto& old:opened){if(!old.handle)continue;bool still=false;for(const auto& set:inventory.swaps)if(set.generation==old.generation)for(auto h:set.handles)if(h==old.handle)still=true;if(!still){release(old.texture);old={};}}
        for(const auto& set:inventory.swaps)if(set.generation){
            SharedTextureShape shape{};if(!decodeSwapTextureSetDesc(set.description,shape))continue;
            for(auto handle:set.handles){bool exists=false;for(const auto& old:opened)exists|=old.handle==handle&&old.generation==set.generation;if(exists)continue;
            ID3D11Texture2D* texture=nullptr;
            if(FAILED(device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)),IID_ID3D11Texture2D,reinterpret_cast<void**>(&texture)))){gates.refuse(CoverageGate::SharedResource);continue;}
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            if(!sharedTextureDescriptionMatches(desc,shape)){release(texture);continue;}
            for(auto& slot:opened)if(!slot.handle){slot={handle,set.generation,set.processId,texture};texture=nullptr;break;}
            release(texture);
            }
        }
        inventoryEpoch=inventory.epoch;prepared=target&&targetView&&blend;
    }
    bool luma(ID3D11DeviceContext* ctx,UINT count,UINT start,const DirectFrame& frame){
        if(!prepared)return gates.refuse(CoverageGate::Prepared);
        if(count!=4||start)return gates.refuse(CoverageGate::DrawShape);
        if(frame.registryEpoch!=inventoryEpoch||frame.presentSerial==failedSerial)return gates.refuse(CoverageGate::FrameEpoch);
        if(activeSerial!=frame.presentSerial){activeSerial=frame.presentSerial;activeEpoch=frame.registryEpoch;drawBits=0;activeContract=0;invalid=false;activePid=0;release(nativeOutput);const float one[4]{1,1,1,1};ctx->ClearRenderTargetView(targetView,one);}
        if(invalid||activeEpoch!=frame.registryEpoch)return gates.refuse(CoverageGate::FrameEpoch);
        ID3D11Buffer* buffer=nullptr;ctx->PSGetConstantBuffers(2,1,&buffer);Constant* cb=nullptr;for(auto& entry:constants)if(entry.valid&&entry.present==frame.presentSerial&&same(entry.resource,buffer))cb=&entry;release(buffer);if(!cb)return gates.refuse(CoverageGate::ConstantBuffer);
        float eyeValue=0;std::memcpy(&eyeValue,cb->bytes.data()+76,sizeof eyeValue);if(!std::isfinite(eyeValue)||(eyeValue!=0&&eyeValue!=1))return gates.refuse(CoverageGate::EyeSelector);const unsigned eye=static_cast<unsigned>(eyeValue);
        ID3D11ShaderResourceView* view=nullptr;ctx->PSGetShaderResources(0,1,&view);ID3D11Resource* resource=nullptr;D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        if(view){view->GetResource(&resource);view->GetDesc(&viewDesc);}release(view);if(!resource)return gates.refuse(CoverageGate::SourceView);
        ID3D11Texture2D* sourceTexture=nullptr;resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&sourceTexture));D3D11_TEXTURE2D_DESC sourceDesc{};if(sourceTexture)sourceTexture->GetDesc(&sourceDesc);release(sourceTexture);
        gates.source(sourceDesc.Width,sourceDesc.Height,static_cast<std::uint32_t>(viewDesc.Format));
        const bool rgba=viewDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||viewDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra=viewDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||viewDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if((!rgba&&!bgra)||sourceDesc.ArraySize!=1||sourceDesc.MipLevels!=1||sourceDesc.SampleDesc.Count!=1||viewDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||viewDesc.Texture2D.MostDetailedMip){release(resource);return gates.refuse(CoverageGate::SourceShape);}
        unsigned sourceBit=0;std::uint32_t pid=0;
        for(unsigned layer=0;layer<frame.layerCount;++layer){const auto& requested=frame.layers[layer][eye];SharedTextureShape shape{};if(!decodeSwapTextureSetDesc(requested.description,shape))continue;for(const auto& known:opened)if(known.handle==requested.submitted.hTexture&&known.generation==requested.swapGeneration&&submittedSharedResource(resource,known.texture,{known.handle,known.generation,known.pid},{requested.submitted.hTexture,requested.swapGeneration,requested.processId},shape)){if(sourceBit){release(resource);return gates.refuse(CoverageGate::AmbiguousSource);}sourceBit=1u<<(2*layer+eye);pid=requested.processId;}}
        if(!sourceBit){release(resource);return gates.refuse(CoverageGate::SharedResource);}
        if(drawBits&sourceBit){release(resource);return gates.refuse(CoverageGate::RepeatedSource);}
        if(activePid&&activePid!=pid){release(resource);return gates.refuse(CoverageGate::MixedProcess);}
        const auto declared=sourceContract.snapshot(pid);
        if(!drawBits)activeContract=declared;
        else if(activeContract!=declared)activeContract=0;
        release(resource);

        ID3D11RenderTargetView* out=nullptr;ID3D11DepthStencilView* depth=nullptr;ctx->OMGetRenderTargets(1,&out,&depth);if(depth){release(depth);release(out);return gates.refuse(CoverageGate::OutputDepth);}
        ID3D11Resource* outputResource=nullptr;if(out)out->GetResource(&outputResource);release(out);
        ID3D11Texture2D* outputTexture=nullptr;if(outputResource)outputResource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&outputTexture));D3D11_TEXTURE2D_DESC desc{};if(outputTexture)outputTexture->GetDesc(&desc);release(outputTexture);
        gates.output(desc.Width,desc.Height,static_cast<std::uint32_t>(desc.Format));
        bool okay=outputResource&&desc.Width==width&&desc.Height==height&&(desc.Format==DXGI_FORMAT_NV12||desc.Format==DXGI_FORMAT_P010)&&(!nativeOutput||same(nativeOutput,outputResource));
        if(okay&&!nativeOutput)nativeOutput=identity(outputResource);
        release(outputResource);if(!okay)return gates.refuse(CoverageGate::OutputShape);
        if(!drawTransmittance(ctx,targetView,blend,forwardDraw,count,start))return gates.refuse(CoverageGate::BlendDraw);
        drawBits|=sourceBit;activePid=pid;gates.signal(CoverageSignal::EyeDraws);
        if(drawBits==((1u<<(2*frame.layerCount))-1)){preparedPid=pid;if(state!=7)state=6;gates.signal(CoverageSignal::CompletePairs);}return true;
    }
    static void STDMETHODCALLTYPE draw(ID3D11DeviceContext* ctx,UINT count,UINT start){
        auto& s=*instance.load();const auto* frame=DirectModeBoundary::currentFrame();s.gates.signal(CoverageSignal::HookDraws);
        const bool caller=s.site(__builtin_return_address(0),0x9f3865);
        if(caller)s.gates.signal(CoverageSignal::NativeDraws);else s.gates.signal(CoverageSignal::OtherDraws);
        const bool ours=ctx==s.context&&s.enabled&&caller&&frame;
        if(!ours){
            if(caller){if(ctx!=s.context)s.gates.refuse(CoverageGate::Context);else if(!s.enabled)s.gates.refuse(CoverageGate::Prepared);else s.gates.refuse(CoverageGate::DirectFrame);}
            forwardDraw(ctx,count,start);return;
        }
        s.gates.signal(CoverageSignal::SourceFrames);
        MultithreadScope protect(s.multithread);forwardDraw(ctx,count,start);
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock){s.gates.refuse(CoverageGate::Lock);s.failedSerial=frame->presentSerial;return;}
        if(!s.luma(ctx,count,start,*frame)){s.invalid=true;s.failedSerial=frame->presentSerial;}
    }
    static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* ctx,ID3D11Resource* resource,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* output){
        auto& s=*instance.load();const auto result=forwardMap(ctx,resource,sub,type,flags,output);
        const bool caller=s.site(__builtin_return_address(0),0x9f36f2);if(caller)s.gates.signal(CoverageSignal::NativeMaps);
        if(ctx!=s.context||!s.enabled||!caller||FAILED(result)||!output||!output->pData||sub||type!=D3D11_MAP_WRITE_DISCARD||flags){
            if(caller)s.gates.refuse(ctx!=s.context?CoverageGate::Context:CoverageGate::ConstantBuffer);
            return result;
        }
        const auto* frame=DirectModeBoundary::currentFrame();if(!frame){s.gates.refuse(CoverageGate::DirectFrame);return result;}
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock){s.gates.refuse(CoverageGate::Lock);s.failedSerial=frame->presentSerial;return result;}
        ID3D11Buffer* buffer=nullptr;if(resource)resource->QueryInterface(IID_ID3D11Buffer,reinterpret_cast<void**>(&buffer));D3D11_BUFFER_DESC desc{};if(buffer)buffer->GetDesc(&desc);release(buffer);
        s.gates.constantBytes(desc.ByteWidth);
        if(desc.ByteWidth!=128||!(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER)){s.gates.refuse(CoverageGate::ConstantBuffer);return result;}
        Constant* found=nullptr;for(auto& c:s.constants)if(same(c.resource,resource))found=&c;for(auto& c:s.constants)if(!found&&!c.resource){c.resource=resource;resource->AddRef();found=&c;}
        if(found){found->valid=false;found->pointer=output->pData;found->present=frame->presentSerial;s.gates.signal(CoverageSignal::ConstantMaps);}else s.gates.refuse(CoverageGate::ConstantBuffer);return result;
    }
    static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* ctx,ID3D11Resource* resource,UINT sub){
        auto& s=*instance.load();if(ctx==s.context&&s.enabled&&s.site(__builtin_return_address(0),0x9f372c)&&!sub){std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(lock)for(auto& c:s.constants)if(c.pointer&&same(c.resource,resource)){std::memcpy(c.bytes.data(),c.pointer,c.bytes.size());c.pointer=nullptr;c.valid=true;s.gates.signal(CoverageSignal::ConstantCaptures);}}
        forwardUnmap(ctx,resource,sub);
    }
    static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* ctx,ID3D11Resource* destination,ID3D11Resource* source){
        auto& s=*instance.load();const auto* frame=DirectModeBoundary::currentFrame();
        const bool caller=s.site(__builtin_return_address(0),0xa217fb);if(caller)s.gates.signal(CoverageSignal::NativeCopies);
        // Every refusal still forwards the native operation exactly once.
        // No source, destination, format or resource content is rewritten.
        if(ctx!=s.context||!s.enabled||!caller||!frame){if(caller)s.gates.refuse(ctx!=s.context?CoverageGate::Context:CoverageGate::DirectFrame);forwardCopy(ctx,destination,source);return;}
        std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock){s.gates.refuse(CoverageGate::Lock);forwardCopy(ctx,destination,source);return;}
        if(!s.prepared||s.invalid||frame->presentSerial!=s.activeSerial||frame->presentSerial==s.failedSerial||frame->registryEpoch!=s.activeEpoch||!same(source,s.nativeOutput)||s.drawBits!=((1u<<(2*frame->layerCount))-1)){s.gates.refuse(CoverageGate::CopyPair);forwardCopy(ctx,destination,source);return;}
        if(!s.sourceContract.current(s.activeContract,s.activePid)){s.gates.refuse(CoverageGate::SourceContract);forwardCopy(ctx,destination,source);return;}
        auto* input=identity(destination);if(!input){s.gates.refuse(CoverageGate::InputReceipt);forwardCopy(ctx,destination,source);return;}
        // A stage is frozen from the native input handoff through matching
        // Unlock; another accepted frame and cold maintenance cannot evict it.
        auto free=s.stages.capture(reinterpret_cast<std::uintptr_t>(input));
        if(!free){release(input);s.gates.refuse(CoverageGate::StageFull);forwardCopy(ctx,destination,source);return;}
        auto& gpu=free->resources;release(gpu.input);gpu.input=input;
        free->receipt={};auto& receipt=free->receipt;
        receipt.input=reinterpret_cast<std::uintptr_t>(input);receipt.contract=s.activeContract;receipt.present=frame->presentSerial;
        receipt.pid=s.activePid;receipt.width=s.width;receipt.height=s.height;
        MultithreadScope protect(s.multithread);const auto captured=clockMicros();
        // Native NVENC completion is input-resource-specific. Explicitly submit
        // this independent readback and its own EVENT before the handoff; do
        // not infer completion from elapsed time or a finished bitstream.
        receipt.copyMicros=captured;
        const CoveragePublication publication{[](void* value)noexcept{static_cast<Stages::Claim*>(value)->publish(Stages::Phase::Captured);},&free};
        copyCoverageBeforeNative(ctx,destination,source,gpu.texture,s.target,gpu.completion,forwardCopy,forwardEnd,forwardFlush,publication);
        s.gates.submitted();s.gates.signal(CoverageSignal::CapturedCopies);
    }
};
std::atomic<SenderCoverage::Impl*> SenderCoverage::Impl::instance{nullptr};
SenderCoverage::SenderCoverage(DirectModeBoundary& d,NvencBoundary& b,ControlRuntime& c):impl_(new Impl(d,b,c)){
    b.setDeviceObserver({Impl::deviceOpened,impl_.get()});b.setInputObserver({Impl::inputCapture,Impl::inputRetire,impl_.get()});b.setOutputAugmenter({Impl::augment,impl_.get()});
}
SenderCoverage::~SenderCoverage(){stop();(void)impl_.release();} // callbacks/COM retained until process exit
bool SenderCoverage::configure(std::uintptr_t module)noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->module||!module)return false;impl_->module=module;return true;}
void SenderCoverage::tick()noexcept{
    auto& s=*impl_;const auto encoders=s.boundary.encoderStates();
    // Once prepared, an unchanged native encoder/inventory requires no render
    // metadata ownership. The old periodic diagnostic/worker lock alone could
    // invalidate a valid Map/Draw, even with no resource work to perform.
    EncoderState actual{};unsigned count=0;for(const auto& e:encoders)if(e.encoder&&e.hevc){actual=e;++count;}
    // 0.2.7 still copied the entire inventory under its native metadata mutex
    // every 8 ms BEFORE this fast path. A collision could invalidate native
    // Submit/Present, or turn an unavailable snapshot into prepared=false for
    // one tick: an isolated None certificate then flashed the camera opaque.
    // Read only the atomic revision when unchanged. Actual draws still require
    // their exact frame registry epoch, resources and live source contract;
    // this observation never authorizes rendering or retains a changed source.
    if(s.enabled&&s.prepared&&count==1&&actual.width==s.width&&actual.height==s.height&&s.direct.inventoryRevision()==s.inventoryEpoch&&s.dispatchOwned())return;
    const auto inventory=s.direct.inventory();
    std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock||s.stopped||!s.module)return;
    try{
        if(!s.device&&s.pendingDevice){s.device=s.pendingDevice;s.pendingDevice=nullptr;if(!s.installContext()){s.gates.refuse(CoverageGate::Context);s.stopped=true;s.state=3;return;}s.state=4;}
        if(!s.enabled)return;
        if(!s.dispatchOwned()){s.loseDispatch();return;}
        if(count!=1){s.gates.refuse(CoverageGate::Encoder);s.prepared=false;s.preparedPid=0;s.state=4;return;}
        if(s.width!=actual.width||s.height!=actual.height||!s.poolReady){if(!s.allocate(actual.width,actual.height)){s.gates.refuse(CoverageGate::Prepared);s.state=5;return;}}
        s.prepared=s.target&&s.targetView&&s.blend&&s.poolReady;
        s.refresh(inventory);
    }catch(...){s.prepared=false;s.state=5;}
}
void SenderCoverage::sourceProcess(std::uint32_t pid)noexcept{impl_->sourceContract.publish(pid);}
void SenderCoverage::stop()noexcept{
    auto& s=*impl_;s.sourceContract.publish(0);s.enabled=false;s.prepared=false;s.lastPid=0;s.preparedPid=0;s.state=8;
    std::lock_guard<std::mutex> lock(s.mutex);s.stopped=true;
    s.facade.restore();
    // No COM or staging release while a cached callback/borrowed AU can exist.
}
bool SenderCoverage::ready(std::uint32_t pid)const noexcept{return impl_->enabled&&impl_->prepared&&impl_->preparedPid==pid&&impl_->direct.ownsProcess(pid);}
bool SenderCoverage::operational(std::uint32_t pid)const noexcept{return ready(pid)&&impl_->lastPid==pid;}
CoverageGateStats SenderCoverage::gateStats()const noexcept{
    auto& s=*impl_;auto result=s.gates.snapshot();
    // Enabled publishes the immutable context/facade. They are retained until
    // process exit; reading public dispatch metadata needs no render lock.
    if(!s.enabled)return result;
    // A successfully installed context can later acquire another runtime
    // dispatch table. Observe its public interface, without following any
    // private renderer object fields or treating diagnostic reads as proof.
    void** current=nullptr;SIZE_T bytes=0;std::array<void*,48> entries{};
    if(ReadProcessMemory(GetCurrentProcess(),s.context,&current,sizeof current,&bytes)&&bytes==sizeof current&&current&&
       ReadProcessMemory(GetCurrentProcess(),current,entries.data(),sizeof entries,&bytes)&&bytes==sizeof entries){
        result.contextTableReadable=true;result.contextTableStable=s.facade.installed();
        const std::array<unsigned,4> indices{{13,14,15,47}};
        const std::array<void*,4> expected{{bits<void*>(&Impl::draw),bits<void*>(&Impl::map),bits<void*>(&Impl::unmap),bits<void*>(&Impl::copy)}};
        const std::array<void*,4> original{{reinterpret_cast<void*>(s.facade.originalFunction(13)),reinterpret_cast<void*>(s.facade.originalFunction(14)),reinterpret_cast<void*>(s.facade.originalFunction(15)),reinterpret_cast<void*>(s.facade.originalFunction(47))}};
        for(unsigned i=0;i<indices.size();++i){if(entries[indices[i]]==expected[i])result.contextHookSlots|=1u<<i;
            if(entries[indices[i]]==original[i])result.contextOriginalSlots|=1u<<i;}
    }
    result.contextFacadeSlots=s.facade.slotCount();result.nativeDispatchMutations=s.facade.nativeMutationCount();
    result.nativeDeviceSame=s.device&&s.device==s.nativeDevicePointer;return result;
}
const char* SenderCoverage::stateName()const noexcept{
    switch(impl_->state.load()){case 0:return "coverage-waiting-native-device";case 1:return "coverage-unsupported-device";case 2:return "coverage-device-observed";case 3:return "coverage-context-or-hook-refused";case 4:return "coverage-waiting-supported-encoder-or-source";case 5:return "coverage-pool-preparation-refused";case 6:return "coverage-source-pair-prepared-waiting-raw-receipt";case 7:return "coverage-native-receipt-certified-not-selected-proof";case 9:return "coverage-public-context-dispatch-changed";default:return "coverage-stopped";}
}
} // namespace stream_bridge::sender
