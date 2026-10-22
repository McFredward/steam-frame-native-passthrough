#include "stream_bridge_compression.h"
#include "stream_bridge_frame.h"
#include <algorithm>
#include <climits>
#include "../third_party/zstd/zstd.h"

// Keep the pristine upstream implementation private. Neither injected
// library exports/interposes another process's LZ4.
// Its default compressor uses a bounded 16-KiB stack table, no heap state.
#define LZ4LIB_VISIBILITY static
#define LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION 1
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC visibility push(hidden)
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4505)
#endif
#include "../third_party/lz4/lz4.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC visibility pop
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace stream_bridge {
bool compressCoverageRle(Bytes rle,std::vector<std::uint8_t>& output){
    if(!rle.data||!rle.size||rle.size%2||rle.size>MaximumCoverageBytes||rle.size>INT_MAX)return false;
    // A compressed representation is only useful when its prefix and complete
    // block are strictly smaller. No output larger than the old RLE is allocated.
    if(rle.size<=5)return false;
    std::vector<std::uint8_t> result(rle.size-1);
    const auto capacity=static_cast<int>(result.size()-4);
    const auto count=LZ4_compress_default(reinterpret_cast<const char*>(rle.data),reinterpret_cast<char*>(result.data()+4),static_cast<int>(rle.size),capacity);
    if(count<=0)return false;
    for(unsigned i=0;i<4;++i)result[i]=static_cast<std::uint8_t>(rle.size>>(8*i));
    result.resize(4+static_cast<std::size_t>(count));output.swap(result);return true;
}
bool decompressCoverageRle(Bytes encoded,std::size_t maskBytes,std::vector<std::uint8_t>& output){
    if(!encoded.data||encoded.size<=4||encoded.size>MaximumCoverageBytes||!maskBytes||maskBytes>MaximumCoverageBytes)return false;
    std::size_t length=0;for(unsigned i=0;i<4;++i)length|=std::size_t(encoded.data[i])<<(8*i);
    // Every RLE pair describes between one and 255 pixels. Check both ends
    // before allocation, including the whole-format encoded-byte ceiling.
    const auto minimum=2*((maskBytes+254)/255);
    const auto maximum=std::min(MaximumCoverageBytes,maskBytes*2);
    if(!length||length%2||length<minimum||length>maximum||length>INT_MAX||encoded.size-4>INT_MAX)return false;
    std::vector<std::uint8_t> result(length);
    const auto count=LZ4_decompress_safe(reinterpret_cast<const char*>(encoded.data+4),reinterpret_cast<char*>(result.data()),static_cast<int>(encoded.size-4),static_cast<int>(length));
    if(count<0||static_cast<std::size_t>(count)!=length)return false;
    std::size_t pixels=0;
    for(std::size_t i=0;i<length;i+=2){if(!result[i]||result[i]>maskBytes-pixels)return false;pixels+=result[i];}
    if(pixels!=maskBytes)return false;
    output.swap(result);return true;
}
// Optional independent frames: no dictionary or previous mask is consulted.
// One-shot decompression has fixed context memory; allocation and advertised
// content size must both match descriptor-derived RLE bounds.
bool compressCoverageRleZstd(Bytes rle,std::size_t maximum,std::vector<std::uint8_t>& output){
    if(!rle.data||!rle.size||rle.size%2||rle.size>MaximumEntropyRleBytes||maximum<=5)return false;
    maximum=std::min(maximum,std::min(MaximumCoverageBytes,rle.size-1));
    if(maximum<=5)return false;
    std::vector<std::uint8_t> result(maximum);
    const auto count=ZSTD_compress(result.data()+4,result.size()-4,rle.data,rle.size,1);
    if(ZSTD_isError(count))return false;
    for(unsigned i=0;i<4;++i)result[i]=static_cast<std::uint8_t>(rle.size>>(8*i));
    result.resize(4+count);output.swap(result);return true;
}
bool decompressCoverageRleZstd(Bytes encoded,std::size_t maskBytes,std::vector<std::uint8_t>& output){
    if(!encoded.data||encoded.size<=4||encoded.size>MaximumCoverageBytes||!maskBytes||maskBytes>MaximumCoverageBytes)return false;
    std::size_t length=0;for(unsigned i=0;i<4;++i)length|=std::size_t(encoded.data[i])<<(8*i);
    const auto minimum=2*((maskBytes+254)/255),maximum=std::min(MaximumCoverageBytes,maskBytes*2);
    if(!length||length%2||length<minimum||length>maximum||length>MaximumEntropyRleBytes)return false;
    const auto* block=encoded.data+4;const auto size=encoded.size-4;
    if(ZSTD_getFrameContentSize(block,size)!=length||ZSTD_getDictID_fromFrame(block,size)!=0)return false;
    const auto frameSize=ZSTD_findFrameCompressedSize(block,size);
    if(ZSTD_isError(frameSize)||frameSize!=size)return false;
    std::vector<std::uint8_t> result(length);
    const auto count=ZSTD_decompress(result.data(),result.size(),block,size);
    if(ZSTD_isError(count)||count!=length)return false;
    std::size_t pixels=0;
    for(std::size_t i=0;i<length;i+=2){if(!result[i]||result[i]>maskBytes-pixels)return false;pixels+=result[i];}
    if(pixels!=maskBytes)return false;
    output.swap(result);return true;
}
// Level one preserves entropy compression on detailed masks whose RLE expansion
// is large. The selector bounds input size; opacity bytes remain unchanged.
bool compressCoverageRawZstd(Bytes raw,std::size_t maximum,std::vector<std::uint8_t>& output){
    if(!raw.data||!raw.size||raw.size>MaximumEntropyRawBytes||maximum<=5)return false;
    maximum=std::min(maximum,raw.size-1);if(maximum<=5)return false;
    std::vector<std::uint8_t> result(maximum);
    const auto count=ZSTD_compress(result.data()+4,result.size()-4,raw.data,raw.size,1);
    if(ZSTD_isError(count))return false;
    for(unsigned i=0;i<4;++i)result[i]=static_cast<std::uint8_t>(raw.size>>(8*i));
    result.resize(4+count);output.swap(result);return true;
}
bool decompressCoverageRawZstd(Bytes encoded,std::size_t maskBytes,std::vector<std::uint8_t>& output){
    if(!encoded.data||encoded.size<=4||encoded.size>MaximumEntropyRawBytes||!maskBytes||maskBytes>MaximumEntropyRawBytes)return false;
    std::size_t length=0;for(unsigned i=0;i<4;++i)length|=std::size_t(encoded.data[i])<<(8*i);
    if(length!=maskBytes)return false;
    const auto* block=encoded.data+4;const auto size=encoded.size-4;
    if(ZSTD_getFrameContentSize(block,size)!=length||ZSTD_getDictID_fromFrame(block,size)!=0)return false;
    const auto frameSize=ZSTD_findFrameCompressedSize(block,size);
    if(ZSTD_isError(frameSize)||frameSize!=size)return false;
    std::vector<std::uint8_t> result(length);
    const auto count=ZSTD_decompress(result.data(),result.size(),block,size);
    if(ZSTD_isError(count)||count!=length)return false;
    output.swap(result);return true;
}
} // namespace stream_bridge
