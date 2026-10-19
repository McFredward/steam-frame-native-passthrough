// Count calls around the unmodified frame implementation, not elapsed time on
// a noisy host. Production contains no test counter or retained decoded cache.
#include "bridge/stream_bridge_compression.h"
namespace stream_bridge {
unsigned observedDecompressions=0;
bool countedDecompression(Bytes input,std::size_t samples,std::vector<std::uint8_t>& output){
    ++observedDecompressions;return decompressCoverageRle(input,samples,output);
}
}
#define decompressCoverageRle countedDecompression
#ifndef SFNP_FRAME_SOURCE
#define SFNP_FRAME_SOURCE "../bridge/stream_bridge_frame.cpp"
#endif
#include SFNP_FRAME_SOURCE
#undef decompressCoverageRle
#include <cstdio>
#include <stdexcept>
namespace sb=stream_bridge;
namespace {
unsigned checks=0;
void check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
sb::Bytes bytes(const std::vector<std::uint8_t>& value){return {value.data(),value.size()};}
void run(){
    sb::Pairing producer,receiver;producer.role=sb::Role::Producer;receiver.role=sb::Role::Receiver;
    producer.pairId.fill(41);receiver.pairId=producer.pairId;producer.key.fill(89);receiver.key=producer.key;
    sb::Nonce binding{},connection{};binding.fill(17);connection.fill(51);sb::AuthSession sender,observer;
    check(sender.prepareProducer(producer,binding)==sb::AuthStatus::Ok&&observer.prepareReceiver(receiver,{connection,7})==sb::AuthStatus::Ok,"independent producer and receiver authentication prepared");
    std::vector<std::uint8_t> expected(64*32);for(unsigned i=0;i<expected.size();++i)expected[i]=static_cast<std::uint8_t>(((i/16)%4)*85);
    const std::vector<std::uint8_t> video{0,0,0,1,0x40,1,0x91,0x80,0,0,1,0x02,1,0x80,0x42,0x80,0,0};
    sb::CoverageEncoding encoding;std::vector<std::uint8_t> compressed,complete,metadata;
    check(sb::encodeBestCoverage(bytes(expected),true,encoding,compressed)&&encoding==sb::CoverageEncoding::RleLz4,"real fractional mask selects lossless block compression");
    check(sb::appendCertifiedFrame(bytes(video),{9,7,64,32},encoding,bytes(compressed),sender,complete)==sb::FrameStatus::Ok,"actual AU certificate signed");
    sb::CertifiedAccessUnit full;check(sb::inspectCertifiedFrame(bytes(complete),observer,full)==sb::FrameStatus::Ok&&sb::serializeCoverageMetadata(full,metadata),"full native AU independently verified before local metadata extraction");
    sb::CertifiedAccessUnit certificate;std::vector<std::uint8_t> samples;sb::CoverageRange range;
    sb::observedDecompressions=0;
    check(sb::inspectCoverageMetadata(bytes(metadata),observer,certificate)==sb::FrameStatus::Ok&&sb::decodeCertifiedCoverage(certificate,samples,range)&&samples==expected,"legacy validation then renderer decode remains byte exact");
    check(sb::observedDecompressions==2,"causal baseline performs two independent LZ4 expansions");
    sb::observedDecompressions=0;
    check(sb::inspectCoverageMetadataDecoded(bytes(metadata),observer,certificate,samples,range)==sb::FrameStatus::Ok&&samples==expected&&range.minimum==0&&range.maximum==255,"combined authenticated expansion retains every fractional sample and content range");
    check(sb::observedDecompressions==1,"combined authenticated compositor expansion performs exactly one LZ4 decode");
    const auto good=metadata;metadata[sb::FrameSignedHeaderBytes]^=1;sb::observedDecompressions=0;samples={31};range={27,39};certificate.descriptor.sourceFrame=901;
    check(sb::inspectCoverageMetadataDecoded(bytes(metadata),observer,certificate,samples,range)==sb::FrameStatus::AuthenticationRefused&&sb::observedDecompressions==0&&samples==std::vector<std::uint8_t>{31}&&range.minimum==27&&range.maximum==39&&certificate.descriptor.sourceFrame==901,"unauthenticated metadata performs zero decompression and cannot publish any transient output");
    metadata=good;metadata.back()^=1;sb::observedDecompressions=0;
    check(sb::inspectCoverageMetadataDecoded(bytes(metadata),observer,certificate,samples,range)==sb::FrameStatus::CoverageMismatch&&sb::observedDecompressions==0&&samples==std::vector<std::uint8_t>{31},"authenticated header with altered coverage hash performs zero decompression");
}
}
int main(){try{run();std::printf("%u authenticated decode cost checks passed\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
