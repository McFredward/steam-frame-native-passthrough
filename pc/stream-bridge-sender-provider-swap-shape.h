#pragma once
#include <cstdint>
namespace stream_bridge::sender {
struct SharedTextureShape {
    std::uint32_t width=0,height=0,format=0,samples=0,arraySize=1;
};
// The pinned vrlink native implementation packs its fourth DirectMode descriptor
// word: low three bits are samples; the remaining bits are array size. Zero in
// either field means one. This is a version-pinned ABI, not the public SDK's
// generic nSampleCount contract. Never rewrite the descriptor sent to SteamVR.
// Our coverage proof supports one array slice only; MSAA metadata may be kept,
// but the actual shared-resource coverage matcher separately requires samples1.
inline bool decodePinnedSwapShape(std::uint32_t width,std::uint32_t height,
    std::uint32_t format,std::uint32_t packed,SharedTextureShape& output)noexcept{
    output={};
    const auto rawSamples=packed&7u,rawArrays=packed>>3;
    const auto samples=rawSamples?rawSamples:1u,arraySize=rawArrays?rawArrays:1u;
    if(!width||width>16384||!height||height>16384||!format||arraySize!=1
        ||(samples!=1&&samples!=2&&samples!=4))return false;
    output={width,height,format,samples,arraySize};return true;
}
} // namespace stream_bridge::sender
