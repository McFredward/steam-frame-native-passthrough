#include "pc/stream-bridge-sender-provider-coverage-render.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <chrono>
#include <algorithm>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
template<class T>struct Com {T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->(){return p;}};
constexpr UINT Width=1152,Height=4608;
constexpr std::size_t Pixels=std::size_t(Width)*Height;
// Own bounded real GPU workload. Each thread's result depends on all loop
// iterations and its previous buffer value. This is not a fake Map receipt.
const char Workload[]=R"(
RWStructuredBuffer<uint> values:register(u0);
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){
 uint x=values[id.x]; [loop] for(uint i=0;i<50000;i++) x=x*1664525u+1013904223u;
 values[id.x]=x;
})";
struct Fixture {
 ID3D11DeviceContext* context=nullptr;ID3D11Resource* nativeDestination=nullptr;
 ID3D11Resource* nativeSource=nullptr;ID3D11Resource* staging=nullptr;ID3D11Resource* coverage=nullptr;
 ID3D11Query* nativeReady=nullptr;ID3D11Query* workDone=nullptr;
 NativeCopy originalCopy=nullptr;NativeMap originalMap=nullptr;NativeUnmap originalUnmap=nullptr;
 unsigned nativeCalls=0,coverageCalls=0,maps=0,unmaps=0;bool workload=true,exact=true;
 HRESULT lastResult=E_FAIL;
};
Fixture* active=nullptr;
void STDMETHODCALLTYPE nativeCopy(ID3D11DeviceContext* context,ID3D11Resource* destination,ID3D11Resource* source){
 auto& f=*active;f.exact=f.exact&&context==f.context;
 if(destination==f.nativeDestination){
  ++f.nativeCalls;f.exact=f.exact&&source==f.nativeSource;
  f.originalCopy(context,destination,source);
  // This is an explicit fixture native-input handoff model, not NVENC itself.
  // Completion at this point includes commands preceding the original copy.
  context->End(f.nativeReady);
  if(f.workload){context->Dispatch(64,1,1);context->End(f.workDone);}
 }else{
  ++f.coverageCalls;f.exact=f.exact&&destination==f.staging&&source==f.coverage;
  f.originalCopy(context,destination,source);
 }
}
HRESULT STDMETHODCALLTYPE nativeMap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* output){
 auto& f=*active;++f.maps;f.exact=f.exact&&context==f.context&&resource==f.staging&&sub==0&&type==D3D11_MAP_READ&&flags==D3D11_MAP_FLAG_DO_NOT_WAIT;
 f.lastResult=f.originalMap(context,resource,sub,type,flags,output);return f.lastResult;
}
void STDMETHODCALLTYPE nativeUnmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub){
 auto& f=*active;++f.unmaps;f.exact=f.exact&&context==f.context&&resource==f.staging&&sub==0;f.originalUnmap(context,resource,sub);
}
void wait(ID3D11DeviceContext* context,ID3D11Query* query){
 const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 BOOL ready=FALSE;
 for(;;){const auto result=context->GetData(query,&ready,sizeof ready,D3D11_ASYNC_GETDATA_DONOTFLUSH);
  check(SUCCEEDED(result),"actual GPU query remains valid");if(result==S_OK&&ready)return;
  check(std::chrono::steady_clock::now()<until,"bounded fixture GPU completion");SwitchToThread();}
}
template<class T>T function(void** table,unsigned slot){T result{};static_assert(sizeof result==sizeof(void*),"public Win64 ABI");std::memcpy(&result,table+slot,sizeof result);return result;}
void run(){
 Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
 check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,context.out()))&&level>=D3D_FEATURE_LEVEL_11_0,"actual WARP device required; no simulated-GPU fallback");
 Com<ID3D11Multithread> multithread;check(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(multithread.out()))),"actual immediate-context protection");multithread->SetMultithreadProtected(TRUE);
 Com<ID3DBlob> computeBytes;check(SUCCEEDED(D3DCompile(Workload,sizeof Workload,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,computeBytes.out(),nullptr)),"own bounded GPU workload compiled");
 Com<ID3D11ComputeShader> compute;check(SUCCEEDED(device->CreateComputeShader(computeBytes->GetBufferPointer(),computeBytes->GetBufferSize(),nullptr,compute.out())),"actual GPU workload shader");
 std::array<UINT,4096> seeds{};for(UINT i=0;i<static_cast<UINT>(seeds.size());++i)seeds[i]=i*17+13;
 D3D11_BUFFER_DESC bd{};bd.ByteWidth=static_cast<UINT>(sizeof seeds);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=sizeof(UINT);
 D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=seeds.data();Com<ID3D11Buffer> work;check(SUCCEEDED(device->CreateBuffer(&bd,&initial,work.out())),"actual bounded workload buffer");
 D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=static_cast<UINT>(seeds.size());Com<ID3D11UnorderedAccessView> uav;check(SUCCEEDED(device->CreateUnorderedAccessView(work.p,&ud,uav.out())),"actual workload UAV");context->CSSetShader(compute.p,nullptr,0);context->CSSetUnorderedAccessViews(0,1,&uav.p,nullptr);
 D3D11_QUERY_DESC qd{};qd.Query=D3D11_QUERY_EVENT;Com<ID3D11Query> nativeReady,workDone;check(SUCCEEDED(device->CreateQuery(&qd,nativeReady.out()))&&SUCCEEDED(device->CreateQuery(&qd,workDone.out())),"actual GPU handoff/completion queries");
 // Warm compilation before testing the asynchronous ordering window.
 context->Dispatch(64,1,1);context->End(workDone.p);context->Flush();wait(context.p,workDone.p);
 D3D11_TEXTURE2D_DESC desc{};desc.Width=Width;desc.Height=Height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
 Com<ID3D11Texture2D> coverage,staging;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,coverage.out())),"full native extent R8 coverage");Com<ID3D11RenderTargetView> rtv;check(SUCCEEDED(device->CreateRenderTargetView(coverage.p,nullptr,rtv.out())),"actual coverage clear target");
 desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,staging.out())),"full native extent R8 staging");
 desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.BindFlags=0;Com<ID3D11Texture2D> nativeSource,nativeDestination;
 std::vector<UINT> colours(Pixels,0x7f8153a7u);initial.pSysMem=colours.data();initial.SysMemPitch=Width*4;
 check(SUCCEEDED(device->CreateTexture2D(&desc,&initial,nativeSource.out()))&&SUCCEEDED(device->CreateTexture2D(&desc,nullptr,nativeDestination.out())),"independent full-size native colour source/destination");
 auto** table=*reinterpret_cast<void***>(context.p);Fixture f;f.context=context.p;f.nativeDestination=nativeDestination.p;f.nativeSource=nativeSource.p;f.staging=staging.p;f.coverage=coverage.p;f.nativeReady=nativeReady.p;f.workDone=workDone.p;f.originalCopy=function<NativeCopy>(table,47);f.originalMap=function<NativeMap>(table,14);f.originalUnmap=function<NativeUnmap>(table,15);active=&f;
 std::vector<std::uint8_t> pixels(Pixels);unsigned pendingWindows=0;
 for(unsigned round=0;round<4;++round){
  const FLOAT value=round&1u?0.75f:0.25f;const FLOAT clear[4]{value,value,value,value};context->ClearRenderTargetView(rtv.p,clear);
  multithread->Enter();copyCoverageBeforeNative(context.p,nativeDestination.p,nativeSource.p,staging.p,coverage.p,nativeCopy);multithread->Leave();
  // Only the fixture flushes/waits. Production copy/readback never does.
  context->Flush();wait(context.p,nativeReady.p);
  BOOL completed=FALSE;const auto completion=context->GetData(workDone.p,&completed,sizeof completed,D3D11_ASYNC_GETDATA_DONOTFLUSH);check(SUCCEEDED(completion),"actual workload availability query");if(completion==S_FALSE||!completed)++pendingWindows;
  std::fill(pixels.begin(),pixels.end(),std::uint8_t{0x5a});const auto result=readCoverageNonblocking(context.p,staging.p,Width,Height,pixels.data(),pixels.size(),nativeMap,nativeUnmap);
  std::printf("round%u: native-handoff complete; later-work-pending=%u; production-Map-hr=0x%08lx\n",round,unsigned(completion==S_FALSE||!completed),static_cast<unsigned long>(result));
  check(SUCCEEDED(result),"coverage ready at native handoff using production DO_NOT_WAIT readback");
  const unsigned expected=round&1u?64u:191u;for(const auto v:pixels)check(v==expected,"exact full-size current-frame coverage; no previous-frame mask");
  wait(context.p,workDone.p);
 }
 check(pendingWindows!=0,"real asynchronous GPU window observed; no manufactured readiness proof");
 check(f.nativeCalls==4&&f.coverageCalls==4&&f.maps==4&&f.unmaps==4&&f.exact,"production dispatch forwards exact native and companion calls once; Map remains nonblocking");
 f.workload=false;copyCoverageBeforeNative(context.p,nativeDestination.p,nativeSource.p,nullptr,nullptr,nativeCopy);context->Flush();wait(context.p,nativeReady.p);
 check(f.nativeCalls==5&&f.coverageCalls==4&&f.exact,"refused companion still forwards exact native copy once");
 desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Com<ID3D11Texture2D> nativeReadback;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,nativeReadback.out())),"native RGB validation staging");f.originalCopy(context.p,nativeReadback.p,nativeDestination.p);
 D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(f.originalMap(context.p,nativeReadback.p,0,D3D11_MAP_READ,0,&mapped)),"fixture-only blocking validation of unchanged native RGB");
 for(UINT y=0;y<Height;++y){const auto* row=reinterpret_cast<const UINT*>(static_cast<const unsigned char*>(mapped.pData)+std::size_t(y)*mapped.RowPitch);for(UINT x=0;x<Width;++x)check(row[x]==0x7f8153a7u,"all ordinary colour bytes remain unchanged");}f.originalUnmap(context.p,nativeReadback.p,0);
 active=nullptr;
 std::printf("actual full-size immediate-context handoff/readback: %u checks PASS; asynchronous windows=%u; native NVENC hardware not exercised\n",checks,pendingWindows);
}
}
int main(){try{run();return 0;}catch(const std::exception& e){std::fprintf(stderr,"full-size GPU readback failed after %u: %s\n",checks,e.what());return 1;}}
