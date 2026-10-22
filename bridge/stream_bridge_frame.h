#pragma once
#include "stream_bridge_auth.h"
#include <vector>

namespace stream_bridge {
// Separate from SFA1 control. The certificate binds the exact entire NVENC AU
// (including any control SEI), not a guessed first slice or native slot index.
constexpr std::size_t FrameSignedHeaderBytes=144,FrameCertificateBytes=176;
// Decoded full-luma mask can cover 8192 squared. Encoded certificate still
// obeys the stricter native AU byte budget; incompressible excess is refused.
constexpr std::size_t MaximumCoverageBytes=64u*1024u*1024u;
constexpr std::array<std::uint8_t,16> FrameUuid{{
    0x53,0x46,0x4e,0x50,0x2d,0x46,0x52,0x41,0x4d,0x45,0x2d,0x56,0x31,0,0,1}};
struct FrameDescriptor {
    std::uint64_t sourceFrame=0,encoderGeneration=0;
    std::uint16_t codedWidth=0,codedHeight=0;
};
enum class CoverageEncoding:std::uint8_t { None=0,Raw=1,Rle=2,RleLz4=3,RleZstd=4 };
struct CertifiedAccessUnit {
    FrameDescriptor descriptor{};
    Nonce binding{};
    CoverageEncoding coverageEncoding=CoverageEncoding::None;
    std::vector<std::uint8_t> original,coverage;
    std::array<std::uint8_t,FrameCertificateBytes> signedHeader{};
    bool originalAccessUnitVerified=false;
};
struct CoverageRange {std::uint8_t minimum=255,maximum=0;};
enum class FrameStatus { Ok,NotFound,InvalidArgument,Malformed,LimitExceeded,
    DuplicateOwned,OriginalMismatch,CoverageMismatch,AuthenticationRefused };
// On failure outputs remain unchanged. No source/native/GPU association is
// inferred here. Empty coverage certifies bytes ONLY and never permits MR.
FrameStatus appendCertifiedFrame(Bytes original,const FrameDescriptor&,
    CoverageEncoding,Bytes encodedCoverage,AuthSession&,std::vector<std::uint8_t>& output);
FrameStatus inspectCertifiedFrame(Bytes observed,AuthSession&,CertifiedAccessUnit& output);
// Detached local IPC: no VCL/video bytes. Validates mask metadata, NOT an AU.
// Only a source-pinned peer plus a current native shared resource lease can
// connect this to a selected picture. Never promote this into full-AU proof.
bool serializeCoverageMetadata(const CertifiedAccessUnit&,std::vector<std::uint8_t>&);
FrameStatus inspectCoverageMetadata(Bytes,AuthSession&,CertifiedAccessUnit&);
// Authenticates before decoding and publishes all outputs transactionally.
// Samples are transient caller-owned output, not a reusable authorization cache.
// None succeeds with empty samples; it still never grants transparent rendering.
FrameStatus inspectCoverageMetadataDecoded(Bytes,AuthSession&,CertifiedAccessUnit&,
    std::vector<std::uint8_t>& samples,CoverageRange&);
bool encodeCoverageRle(Bytes raw,std::vector<std::uint8_t>& output);
// New encoding is opt-in only after mutual capability negotiation. All modes
// preserve every fractional-alpha sample; the smallest complete form wins.
// Compression/decompression runs on cold workers, never a compositor thread.
bool encodeBestCoverage(Bytes raw,bool allowRleLz4,CoverageEncoding& encoding,std::vector<std::uint8_t>& output,bool allowRleZstd=false);
bool decodeCertifiedCoverage(const CertifiedAccessUnit&,std::vector<std::uint8_t>& output);
bool decodeCertifiedCoverage(const CertifiedAccessUnit&,std::vector<std::uint8_t>& output,CoverageRange&);
} // namespace stream_bridge
