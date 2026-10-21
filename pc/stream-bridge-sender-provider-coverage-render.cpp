#include "stream-bridge-sender-provider-coverage-render.h"
#include <array>
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
    return sharedTextureDescriptionMatches(actual,expected);
}
}
bool nativeLumaTarget(ID3D11DeviceContext* ctx)noexcept{
        ID3D11RenderTargetView* target=nullptr;ID3D11DepthStencilView* depth=nullptr;ctx->OMGetRenderTargets(1,&target,&depth);
        D3D11_RENDER_TARGET_VIEW_DESC view{};if(target)target->GetDesc(&view);
        ID3D11Resource* resource=nullptr;if(target)target->GetResource(&resource);
        ID3D11Texture2D* texture=nullptr;if(resource)resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void**>(&texture));
        D3D11_TEXTURE2D_DESC shape{};if(texture)texture->GetDesc(&shape);
        const bool okay=!depth&&texture&&((shape.Format==DXGI_FORMAT_NV12&&view.Format==DXGI_FORMAT_R8_UNORM)||
            (shape.Format==DXGI_FORMAT_P010&&view.Format==DXGI_FORMAT_R16_UNORM));
        if(texture)texture->Release();
        if(resource)resource->Release();
        if(target)target->Release();
        if(depth)depth->Release();
        return okay;
    }
bool sharedTextureDescriptionMatches(const D3D11_TEXTURE2D_DESC& actual,
    const SharedTextureShape& expected)noexcept{
    return expected.width&&expected.width<=16384&&expected.height&&expected.height<=16384
        &&expected.format&&expected.arraySize==1
        &&(expected.samples==1||expected.samples==2||expected.samples==4)
        &&actual.Width==expected.width&&actual.Height==expected.height
        &&actual.Format==static_cast<DXGI_FORMAT>(expected.format)
        &&actual.SampleDesc.Count==expected.samples&&actual.ArraySize==expected.arraySize&&actual.MipLevels==1;
}
bool submittedSharedResource(ID3D11Resource* native,ID3D11Texture2D* held,
    const SharedTextureOwner& opened,const SharedTextureOwner& submitted,const SharedTextureShape& shape)noexcept{
    if(!native||!held||!submitted.handle||!submitted.generation||!submitted.processId
        ||opened.handle!=submitted.handle||opened.generation!=submitted.generation||opened.processId!=submitted.processId
        ||!shape.width||!shape.height||shape.width>16384||shape.height>16384||!shape.format||shape.samples!=1||shape.arraySize!=1)return false;
    return textureShape(native,shape)&&textureShape(held,shape)
        &&allocationHandle(native,submitted.handle)&&allocationHandle(held,submitted.handle);
}
void copyCoverageBeforeNative(ID3D11DeviceContext* context,ID3D11Resource* nativeDestination,
    ID3D11Resource* nativeSource,ID3D11Resource* readback,ID3D11Resource* coverage,ID3D11Query* completion,
    NativeCopy nativeCopy,NativeEnd nativeEnd,NativeFlush nativeFlush,CoveragePublication publication)noexcept{
    if(readback&&coverage&&completion){
        nativeCopy(context,readback,coverage);
        nativeEnd(context,completion);
        nativeFlush(context);
        // Publish the immutable receipt before handing the native input to
        // another encoder thread. Publishing after nativeCopy returns races a
        // consumer which can already observe that native resource handoff.
        if(publication.publish)publication.publish(publication.context);
    }
    nativeCopy(context,nativeDestination,nativeSource);
}
HRESULT readCoverageNonblocking(ID3D11DeviceContext* context,ID3D11Resource* readback,ID3D11Query* completion,
    std::uint32_t width,std::uint32_t height,std::uint8_t* destination,std::size_t capacity,
    NativeGetData nativeGetData,NativeMap nativeMap,NativeUnmap nativeUnmap,CoverageReadbackResult& receipt)noexcept{
    receipt={};
    if(!context||!readback||!completion||!width||!height||!destination||!nativeGetData||!nativeMap||!nativeUnmap||
        std::size_t(width)>capacity/height)return E_INVALIDARG;
    BOOL completed=FALSE;
    const auto ready=nativeGetData(context,completion,&completed,sizeof completed,D3D11_ASYNC_GETDATA_DONOTFLUSH);
    receipt.query=ready==S_OK&&!completed?S_FALSE:ready;
    if(receipt.query!=S_OK)return FAILED(receipt.query)?receipt.query:DXGI_ERROR_WAS_STILL_DRAWING;
    receipt.mapAttempted=true;
    D3D11_MAPPED_SUBRESOURCE data{};
    const auto result=nativeMap(context,readback,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&data);
    if(FAILED(result))return result;
    const bool valid=data.pData&&data.RowPitch>=width;
    if(valid)for(std::uint32_t y=0;y<height;++y){
        const auto* row=static_cast<const std::uint8_t*>(data.pData)+std::size_t(y)*data.RowPitch;
        auto* out=destination+std::size_t(y)*width;
        for(std::uint32_t x=0;x<width;++x)out[x]=std::uint8_t(255-row[x]);
    }
    nativeUnmap(context,readback,0);
    return valid?S_OK:E_INVALIDARG;
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
} // namespace stream_bridge::sender
