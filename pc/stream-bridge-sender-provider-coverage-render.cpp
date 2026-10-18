#include "stream-bridge-sender-provider-coverage-render.h"
#include <array>
#include <cmath>
namespace stream_bridge::sender {
namespace {
bool allocationHandle(ID3D11Resource* resource,std::uint64_t expected)noexcept{
    IDXGIResource* shared=nullptr;
    if(!resource||FAILED(resource->QueryInterface(IID_IDXGIResource,reinterpret_cast<void**>(&shared)))||!shared)return false;
    HANDLE actual=nullptr;const auto result=shared->GetSharedHandle(&actual);shared->Release();
    return SUCCEEDED(result)&&actual&&static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(actual))==expected;
}
bool textureShape(ID3D11Resource* resource,const SharedTextureShape& expected)noexcept{
    ID3D11Texture2D* texture=nullptr;
    if(!resource||FAILED(resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&texture)))||!texture)return false;
    D3D11_TEXTURE2D_DESC actual{};texture->GetDesc(&actual);texture->Release();
    return actual.Width==expected.width&&actual.Height==expected.height&&actual.Format==static_cast<DXGI_FORMAT>(expected.format)
        &&actual.SampleDesc.Count==expected.samples&&actual.ArraySize==1&&actual.MipLevels==1;
}
}
bool submittedSharedResource(ID3D11Resource* native,ID3D11Texture2D* held,
    const SharedTextureOwner& opened,const SharedTextureOwner& submitted,const SharedTextureShape& shape)noexcept{
    if(!native||!held||!submitted.handle||!submitted.generation||!submitted.processId
        ||opened.handle!=submitted.handle||opened.generation!=submitted.generation||opened.processId!=submitted.processId
        ||!shape.width||!shape.height||shape.width>16384||shape.height>16384||!shape.format||shape.samples!=1)return false;
    return textureShape(native,shape)&&textureShape(held,shape)
        &&allocationHandle(native,submitted.handle)&&allocationHandle(held,submitted.handle);
}
bool createTransmittanceBlend(ID3D11Device* device,ID3D11BlendState** output)noexcept{
    if(!device||!output)return false;
    D3D11_BLEND_DESC desc{};auto& rt=desc.RenderTarget[0];rt.BlendEnable=TRUE;
    rt.SrcBlend=D3D11_BLEND_ZERO;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha=D3D11_BLEND_ZERO;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    ID3D11BlendState* blend=nullptr;if(FAILED(device->CreateBlendState(&desc,&blend)))return false;*output=blend;return true;
}
bool drawTransmittance(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,
    ID3D11BlendState* blend,NativeDraw nativeDraw,UINT count,UINT start)noexcept{
    if(!context||!target||!blend||!nativeDraw||!count)return false;
    D3D11_RENDER_TARGET_VIEW_DESC desc{};target->GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_R8_UNORM||desc.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D)return false;
    std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> originals{};ID3D11DepthStencilView* depth=nullptr;
    context->OMGetRenderTargets(static_cast<UINT>(originals.size()),originals.data(),&depth);
    if(depth){depth->Release();for(auto* rt:originals)if(rt)rt->Release();return false;}
    ID3D11BlendState* originalBlend=nullptr;FLOAT factors[4]{};UINT mask=0;
    context->OMGetBlendState(&originalBlend,factors,&mask);
    context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(blend,factors,mask);
    nativeDraw(context,count,start);
    UINT restoredCount=0;for(UINT i=0;i<originals.size();++i)if(originals[i])restoredCount=i+1;
    context->OMSetRenderTargets(restoredCount,originals.data(),nullptr);context->OMSetBlendState(originalBlend,factors,mask);
    if(originalBlend)originalBlend->Release();
    for(auto* rt:originals)if(rt)rt->Release();
    return true;
}
bool copySubmittedInterior(ID3D11DeviceContext* context,ID3D11Texture2D* source,
    float uMin,float vMin,float uMax,float vMax,ID3D11Texture2D* target,unsigned rowIndex)noexcept{
    if(!context||!source||!target||rowIndex>=8||!std::isfinite(uMin)||!std::isfinite(uMax)||!std::isfinite(vMin)||!std::isfinite(vMax)||uMin<0||vMin<0||uMax>1||vMax>1||uMax<=uMin||vMax<=vMin)return false;
    D3D11_TEXTURE2D_DESC from{},to{};source->GetDesc(&from);target->GetDesc(&to);
    auto family=[](DXGI_FORMAT f){switch(f){case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return 1;case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return 2;default:return 0;}};
    if(!family(from.Format)||family(from.Format)!=family(to.Format)||from.ArraySize!=1||from.MipLevels!=1||from.SampleDesc.Count!=1||to.Width!=5||to.Height!=40||to.ArraySize!=1||to.MipLevels!=1||to.SampleDesc.Count!=1)return false;
    const auto minX=static_cast<int>(std::ceil(uMin*from.Width-.5f))+1,maxX=static_cast<int>(std::floor(uMax*from.Width-.5f))-1;
    const auto minY=static_cast<int>(std::ceil(vMin*from.Height-.5f))+1,maxY=static_cast<int>(std::floor(vMax*from.Height-.5f))-1;
    if(minX<0||minY<0||maxX<minX||maxY<minY||maxX>=static_cast<int>(from.Width)||maxY>=static_cast<int>(from.Height))return false;
    for(unsigned y=0;y<5;++y)for(unsigned x=0;x<5;++x){const UINT sx=static_cast<UINT>(minX+(maxX-minX)*static_cast<int>(x)/4),sy=static_cast<UINT>(minY+(maxY-minY)*static_cast<int>(y)/4);D3D11_BOX pixel{sx,sy,0,sx+1,sy+1,1};context->CopySubresourceRegion(target,0,x,rowIndex*5+y,0,source,0,&pixel);}
    return true;
}
bool sourceInteriorTranslucent(const D3D11_MAPPED_SUBRESOURCE& sample,unsigned bits)noexcept{
    if(!sample.pData||sample.RowPitch<20||!bits||(bits&~255u))return false;
    for(unsigned row=0;row<8;++row)if(bits&(1u<<row)){
        bool found=false;for(unsigned y=0;y<5;++y)for(unsigned x=0;x<5;++x){const auto* pixel=static_cast<const std::uint8_t*>(sample.pData)+std::size_t(row*5+y)*sample.RowPitch+4*x;found|=pixel[3]<255;}
        if(!found)return false;
    }
    return true;
}
} // namespace stream_bridge::sender
