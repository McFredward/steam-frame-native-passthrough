#pragma once
#include "stream_bridge_protocol.h"
#include <vector>

namespace stream_bridge {
// Independent lossless blocks. The four-byte little-endian prefix names the
// exact RLE byte count, not a dictionary, prior frame, or image association.
// Call only on a cold worker. Outputs remain unchanged on refusal.
constexpr std::size_t MaximumEntropyRleBytes=1024u*1024u;
// Bound extra encoder/decoder work; larger masks retain the existing codec.
bool compressCoverageRle(Bytes rle,std::vector<std::uint8_t>& output);
bool compressCoverageRleZstd(Bytes rle,std::size_t maximum,std::vector<std::uint8_t>& output);
bool decompressCoverageRleZstd(Bytes encoded,std::size_t maskBytes,std::vector<std::uint8_t>& output);
bool decompressCoverageRle(Bytes encoded,std::size_t maskBytes,std::vector<std::uint8_t>& output);
} // namespace stream_bridge
