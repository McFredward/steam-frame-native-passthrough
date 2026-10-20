#pragma once
#include "stream-bridge-sender-provider-coverage-render.h"
#include "stream-bridge-source-alpha-stats.h"
#include <algorithm>
namespace stream_bridge::sender {
// Caller has proved the exact native shared source / eye / current source grant.
// Fixed small storage is prepared cold. Capture does not modify native state,
// resource pixels, shaders, UVs, source permission or the coverage pipeline.
struct SourceAlphaApi {
    NativeGetData get=nullptr;NativeMap map=nullptr;NativeUnmap unmap=nullptr;
};
class SourceAlphaProbe {
public:
    ~SourceAlphaProbe(){for(auto& slot:slots_){for(auto*& texture:slot.staging)release(texture);release(slot.query);}}
    bool prepare(ID3D11Device* device)noexcept{
        if(attempted_)return ready_;
        attempted_=true;
        if(!device)return fail(E_INVALIDARG);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=32;desc.Height=96;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC query{};query.Query=D3D11_QUERY_EVENT;
        for(auto& slot:slots_){
            for(unsigned i=0;i<2;++i){desc.Format=i?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
                const auto hr=device->CreateTexture2D(&desc,nullptr,&slot.staging[i]);if(FAILED(hr))return fail(hr);}
            const auto hr=device->CreateQuery(&query,&slot.query);if(FAILED(hr))return fail(hr);
        }
        ready_=true;return true;
    }
    void sample(ID3D11DeviceContext* context,ID3D11Resource* source,unsigned eye,
        const SourceAlphaMetadata& metadata,std::uint64_t now,const SourceAlphaApi& api)noexcept{
        if(!ready_||!context||!source||eye>1||!metadata.present||!metadata.pid||!api.get||!api.map||!api.unmap)return;
        auto& slot=slots_[eye];
        if(slot.pending){
            BOOL done=FALSE;auto hr=api.get(context,slot.query,&done,sizeof done,D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE||(hr==S_OK&&!done))return;
            if(hr==S_OK){
                D3D11_MAPPED_SUBRESOURCE data{};
                hr=api.map(context,slot.staging[slot.family],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&data);
                if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return;
                if(hr==S_OK){
                    if(data.pData&&data.RowPitch>=128){
                        SourceAlphaSample result{};result.source=slot.metadata;result.samples=++slot.samples;
                        for(unsigned region=0;region<3;++region){
                            unsigned low=255,high=0,sum=0,zero=0,partial=0;
                            for(unsigned y=region*32;y<(region+1)*32;++y)for(unsigned x=0;x<32;++x){
                                const auto alpha=static_cast<const std::uint8_t*>(data.pData)[std::size_t(y)*data.RowPitch+x*4+3];
                                low=std::min(low,unsigned(alpha));high=std::max(high,unsigned(alpha));sum+=alpha;
                                zero+=alpha==0;partial+=alpha!=0&&alpha!=255;
                            }
                            result.alpha[region]=std::uint64_t(low)|(std::uint64_t(sum/1024)<<8)|(std::uint64_t(high)<<16)
                                |(std::uint64_t(zero)<<24)|(std::uint64_t(partial)<<40);
                        }
                        publications_[eye].publish(result);
                    }else fail(E_INVALIDARG);
                    api.unmap(context,slot.staging[slot.family],0);
                }
            }
            if(FAILED(hr))fail(hr);
            slot.pending=false;
        }
        if(now<slot.next)return;
        const auto format=static_cast<DXGI_FORMAT>(metadata.resourceFormat);
        const bool rgba=format==DXGI_FORMAT_R8G8B8A8_TYPELESS||format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra=format==DXGI_FORMAT_B8G8R8A8_TYPELESS||format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if((!rgba&&!bgra)||metadata.width<32||metadata.height<32||metadata.width>16384||metadata.height>16384)return;
        slot.family=bgra?1:0;
        for(unsigned region=0;region<3;++region){
            const UINT x=(metadata.width-32)/2;
            const UINT y=std::min(metadata.height-32,std::max(16u,metadata.height*(region+1)/4)-16);
            const D3D11_BOX box{x,y,0,x+32,y+32,1};
            context->CopySubresourceRegion(slot.staging[slot.family],0,0,region*32,0,source,0,&box);
        }
        context->End(slot.query);slot.metadata=metadata;slot.pending=true;slot.next=now+2000000;
        // Native submission flushes its ordinary work. This diagnostic never
        // adds a Flush, wait, retry, CPU color export or replacement frame.
    }
    bool read(unsigned eye,SourceAlphaSample& sample)const noexcept{return eye<2&&publications_[eye].read(sample);}
    std::uint32_t error()const noexcept{return error_.load(std::memory_order_relaxed);}
private:
    template<class T>static void release(T*& value)noexcept{if(value){value->Release();value=nullptr;}}
    bool fail(HRESULT error)noexcept{error_.store(static_cast<std::uint32_t>(error),std::memory_order_relaxed);return false;}
    struct Slot {
        std::array<ID3D11Texture2D*,2> staging{};ID3D11Query* query=nullptr;
        SourceAlphaMetadata metadata{};unsigned family=0;bool pending=false;
        std::uint64_t next=0,samples=0;
    };
    bool attempted_=false,ready_=false;
    std::array<Slot,2> slots_{};
    std::array<SourceAlphaPublication,2> publications_{};
    std::atomic<std::uint32_t> error_{};
};
} // namespace stream_bridge::sender
