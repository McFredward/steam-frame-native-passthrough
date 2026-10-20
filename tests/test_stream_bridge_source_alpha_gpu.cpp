#include "pc/stream-bridge-source-alpha-probe.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0,maps=0,queries=0;bool pretendPending=false;
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->(){return p;}};
HRESULT STDMETHODCALLTYPE get(ID3D11DeviceContext* c,ID3D11Asynchronous* q,void* out,UINT n,UINT flags){
    ++queries;check(flags==D3D11_ASYNC_GETDATA_DONOTFLUSH,"diagnostic query never flushes");
    return pretendPending?S_FALSE:c->GetData(q,out,n,flags);
}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE* out){
    ++maps;check(mode==D3D11_MAP_READ&&flags==D3D11_MAP_FLAG_DO_NOT_WAIT,"diagnostic Map is nonblocking read-only");
    return c->Map(r,sub,mode,flags,out);
}
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub){c->Unmap(r,sub);}
void run(){
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,context.out());
    if(FAILED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.out(),&level,context.out());
    check(SUCCEEDED(hr),"real D3D11 device");
    SourceAlphaProbe probe;check(probe.prepare(device.p),"small diagnostic resources prepared cold");
    SourceAlphaSample sample{};check(!probe.read(0,sample),"no result exists before a completed capture");
    SourceAlphaProbe unavailable;check(!unavailable.prepare(nullptr)&&unavailable.error()!=0,"setup failure remains a diagnostic error");
    const SourceAlphaApi api{get,map,unmap};
    const std::array<DXGI_FORMAT,6> formats{{DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}};
    std::uint64_t present=0;
    for(auto format:formats)for(unsigned alpha:{0u,128u,255u}){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=64;desc.Height=128;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=format;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Com<ID3D11Texture2D> source;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,source.out())),"actual RGBA/BGRA source in each supported storage family");
        std::vector<std::array<unsigned char,4>> pixels(desc.Width*desc.Height);
        for(unsigned i=0;i<pixels.size();++i)pixels[i]={{static_cast<unsigned char>(i%251),83,197,static_cast<unsigned char>(alpha)}};
        context->UpdateSubresource(source.p,0,nullptr,pixels.data(),desc.Width*4,0);
        SourceAlphaMetadata metadata{};metadata.present=++present;metadata.pid=42;metadata.width=desc.Width;metadata.height=desc.Height;
        metadata.resourceFormat=format;metadata.viewFormat=format;metadata.uv={{0,0,1,1}};
        metadata.nonFovealAlpha=0;metadata.directFoveated=1;metadata.sharpness=-.2f;
        const std::uint64_t now=present*3000000;
        for(unsigned eye=0;eye<2;++eye){
            const auto beforeQueries=queries,beforeMaps=maps;
            probe.sample(context.p,source.p,eye,metadata,now,api);
            pretendPending=true;probe.sample(context.p,source.p,eye,metadata,now+1,api);pretendPending=false;
            check(queries==beforeQueries+1&&maps==beforeMaps,"pending query returns after one poll without Map or waiting");
        }
        context->Flush(); // Owned test submission; absent from production probe.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
        for(unsigned eye=0;eye<2;++eye){
            for(;;){
                const auto beforeQueries=queries,beforeMaps=maps;
                probe.sample(context.p,source.p,eye,metadata,now+1,api);
                check(queries<=beforeQueries+1&&maps<=beforeMaps+1,"one query and at most one Map per callback");
                if(probe.read(eye,sample)&&sample.source.present==present)break;
                check(std::chrono::steady_clock::now()<deadline,"real source GPU query/readback completes");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(sample.source.pid==42&&sample.source.resourceFormat==unsigned(format)&&sample.source.width==64&&sample.source.height==128,
                "diagnostic PID/frame/extent/format belong to the completed capture");
            check(sample.source.nonFovealAlpha==0&&sample.source.directFoveated==1&&sample.source.sharpness==-.2f,
                "native shader alpha/flags are recorded independently of source alpha");
            for(auto v:sample.alpha){
                check((v&255)==alpha&&((v>>8)&255)==alpha&&((v>>16)&255)==alpha,"actual source alpha preserves zero, fractional and opaque values");
                check(((v>>24)&65535)==(alpha==0?1024:0)&&((v>>40)&65535)==(alpha==128?1024:0),"zero and fractional population counts match actual pixels");
            }
            const auto beforeQueries=queries;probe.sample(context.p,source.p,eye,metadata,now+2,api);
            check(queries==beforeQueries,"completed sample is not recaptured before its two-second cadence");
        }
        // Source pixels, including nonzero RGB at alpha zero, must be untouched.
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> unchanged;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,unchanged.out())),"owned verification staging");
        context->CopyResource(unchanged.p,source.p);D3D11_MAPPED_SUBRESOURCE data{};
        check(SUCCEEDED(context->Map(unchanged.p,0,D3D11_MAP_READ,0,&data)),"owned source verification readback");
        for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)
            check(std::memcmp(static_cast<const unsigned char*>(data.pData)+y*data.RowPitch+x*4,pixels[y*desc.Width+x].data(),4)==0,"diagnostic never rewrites scene RGB/alpha");
        context->Unmap(unchanged.p,0);
        metadata.pid=0;const auto beforeQueries=queries;probe.sample(context.p,source.p,0,metadata,now+3000000,api);
        check(queries==beforeQueries,"missing source PID cannot initiate a diagnostic capture");
    }
    check(probe.error()==0,"actual supported source captures have no GPU errors");
}
}
int main(){try{run();std::printf("actual D3D11 native-source alpha diagnostic: %u checks PASS; no headset test\n",checks);return 0;}
catch(const std::exception& error){std::fprintf(stderr,"native-source alpha diagnostic failed after %u: %s\n",checks,error.what());return 1;}}
