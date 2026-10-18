#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <cstdint>
namespace stream_bridge::sender {
using NativeDraw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
// Native PS output alpha supplies coverage. Its RGB/luma is ignored; the R8
// target stores transmittance T <- T*(1-a), cleared to1 once per native frame.
// The backend emits A=255-T. No independent shader, RGB classification or UV.
bool createTransmittanceBlend(ID3D11Device*,ID3D11BlendState**)noexcept;
// Requires an actual same-device R8 RTV and the proved original luma Draw.
// All native geometry/PS/CB/SRV/sampler/rasterizer/viewport/scissor are retained.
// An attached depth target is refused rather than writing it a second time.
bool drawTransmittance(ID3D11DeviceContext*,ID3D11RenderTargetView*,
    ID3D11BlendState*,NativeDraw,UINT vertexCount,UINT startVertex)noexcept;
// Copies only pixel centres strictly inside the actual submitted source ROI.
// target is a same-format-family5x40 preallocated texture, rowIndex0..7.
bool copySubmittedInterior(ID3D11DeviceContext*,ID3D11Texture2D* source,
    float uMin,float vMin,float uMax,float vMax,ID3D11Texture2D* target,unsigned rowIndex)noexcept;
// Consumes only completed original-public Map output for that immutable receipt.
// Every selected eye/layer row needs an interior alpha witness; one raw eye
// cannot admit another stale atlas eye. Empty/invalid metadata always refuses.
bool sourceInteriorTranslucent(const D3D11_MAPPED_SUBRESOURCE&,unsigned sourceBits)noexcept;
} // namespace stream_bridge::sender
