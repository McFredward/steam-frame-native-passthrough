#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <cstdint>
#include <cstddef>
#include "stream-bridge-sender-provider-swap-shape.h"
namespace stream_bridge::sender {
using NativeCopy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using NativeMap=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using NativeEnd=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*);
using NativeFlush=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using NativeGetData=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*,void*,UINT,UINT);
struct CoverageReadbackResult {HRESULT query=E_PENDING;bool mapAttempted=false;};
using NativeUnmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
// Caller owns the same protected immediate-context interval. The optional
// companion copy, exact event End and asynchronous Flush precede the native
// input copy, forwarded once. Only a fully admitted active-MR capture supplies
// all three resources. Refusal adds no companion commands or Flush.
// Flush submits work; it does not establish completion.
void copyCoverageBeforeNative(ID3D11DeviceContext*,ID3D11Resource* nativeDestination,
    ID3D11Resource* nativeSource,ID3D11Resource* readback,ID3D11Resource* coverage,ID3D11Query*,
    NativeCopy,NativeEnd,NativeFlush)noexcept;
// The exact event must complete before one nonblocking Map, and one Unmap
// follows Map success. One DONOTFLUSH GetData; no Flush, wait,
// retry, allocation or later-frame substitution. Destination receives A=255-T.
HRESULT readCoverageNonblocking(ID3D11DeviceContext*,ID3D11Resource* readback,ID3D11Query*,
    std::uint32_t width,std::uint32_t height,std::uint8_t* destination,std::size_t capacity,
    NativeGetData,NativeMap,NativeUnmap,CoverageReadbackResult&)noexcept;
using NativeDraw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
struct SharedTextureOwner {std::uint64_t handle=0,generation=0;std::uint32_t processId=0;};
// Cold import compares the public kernel texture description against normalized
// pinned DirectMode metadata. This shape check alone grants no frame authority.
bool sharedTextureDescriptionMatches(const D3D11_TEXTURE2D_DESC&,
    const SharedTextureShape&)noexcept;
// Separately opened DXGI resource wrappers have different IUnknown identities.
// Admit only the SAME legacy shared-allocation handle on the actual native SRV
// resource AND the held open, plus the current DirectMode generation/PID/shape.
// The held open keeps the shared handle/allocation alive. No pointer, size-only,
// latest-frame or NT-handle fallback; generation comes from the live registry.
bool submittedSharedResource(ID3D11Resource* native,ID3D11Texture2D* held,
    const SharedTextureOwner& opened,const SharedTextureOwner& submitted,
    const SharedTextureShape&)noexcept;

// Native PS output alpha supplies coverage. Its RGB/luma is ignored; the R8
// target stores transmittance T <- T*(1-a), cleared to1 once per native frame.
// The backend emits A=255-T. No independent shader, RGB classification or UV.
bool createTransmittanceBlend(ID3D11Device*,ID3D11BlendState**)noexcept;
// Requires an actual same-device R8 RTV and the proved original luma Draw.
// All native geometry/PS/CB/SRV/sampler/rasterizer/viewport/scissor are retained.
// An attached depth target is refused rather than writing it a second time.
bool drawTransmittance(ID3D11DeviceContext*,ID3D11RenderTargetView*,
    ID3D11BlendState*,NativeDraw,UINT vertexCount,UINT startVertex)noexcept;
} // namespace stream_bridge::sender
