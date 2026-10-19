#include "bridge/stream_bridge_frame.h"
#include "bridge/stream_bridge_compression.h"
#include "receiver/sha256.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace sb=stream_bridge;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
sb::Bytes bytes(const std::vector<std::uint8_t>& value){return {value.data(),value.size()};}
void length(std::vector<std::uint8_t>& value,std::uint32_t n){for(unsigned i=0;i<4;++i)value[i]=static_cast<std::uint8_t>(n>>(8*i));}
struct Session {
    sb::AuthSession sender,receiver;
    Session(){sb::Pairing pair;pair.pairId.fill(9);pair.key.fill(73);pair.role=sb::Role::Producer;sb::Nonce binding{},connection{};binding.fill(11);connection.fill(17);
        check(sender.prepareProducer(pair,binding)==sb::AuthStatus::Ok,"real compressor producer signer prepared");pair.role=sb::Role::Receiver;
        check(receiver.prepareReceiver(pair,{connection,1})==sb::AuthStatus::Ok,"real independent receiver verifier prepared");}
};
std::vector<std::uint8_t> nativeVideo(){return {0,0,0,1,0x40,1,0x91,0x80,0,0,1,0x02,1,0x80,0x42,0x80,0,0};}
// An authenticated peer may still send malformed compressed bytes. Build that
// exact adversarial metadata without letting appendCertifiedFrame validate it.
std::vector<std::uint8_t> signedMalformed(Session& session,sb::Bytes payload,const sb::FrameDescriptor& descriptor){
    std::vector<std::uint8_t> native;const auto video=nativeVideo();sb::CertifiedAccessUnit certificate;
    check(sb::appendCertifiedFrame(bytes(video),descriptor,sb::CoverageEncoding::None,{},session.sender,native)==sb::FrameStatus::Ok&&sb::inspectCertifiedFrame(bytes(native),session.receiver,certificate)==sb::FrameStatus::Ok,"independent valid header for malformed compressed peer");
    std::vector<std::uint8_t> result(certificate.signedHeader.begin(),certificate.signedHeader.end());result[5]=3;
    for(unsigned i=0;i<4;++i){result[60+i]=static_cast<std::uint8_t>(payload.size>>(8*i));result[68+i]=static_cast<std::uint8_t>((std::size_t(descriptor.codedWidth)*descriptor.codedHeight)>>(8*i));}
    frame_receiver::Sha256 digest;if(payload.size)digest.update(payload.data,payload.size);const auto hash=digest.finish();std::copy(hash.begin(),hash.end(),result.data()+112);
    sb::AuthTag tag;check(session.sender.signFrameHeader({result.data(),sb::FrameSignedHeaderBytes},tag)==sb::AuthStatus::Ok,"malformed compressed peer carries a real current MAC");std::copy(tag.begin(),tag.end(),result.data()+sb::FrameSignedHeaderBytes);
    if(payload.size)result.insert(result.end(),payload.data,payload.data+payload.size);
    return result;
}
void roundTrip(){
    Session session;const auto video=nativeVideo();const sb::FrameDescriptor descriptor{7,3,64,32};
    std::vector<std::uint8_t> raw(64*32);for(std::size_t i=0;i<raw.size();++i)raw[i]=static_cast<std::uint8_t>(((i/16)%4)*85);
    sb::CoverageEncoding encoding=sb::CoverageEncoding::None;std::vector<std::uint8_t> compressed,rle,encoded,decoded;
    check(sb::encodeCoverageRle(bytes(raw),rle)&&sb::encodeBestCoverage(bytes(raw),true,encoding,compressed)&&encoding==sb::CoverageEncoding::RleLz4&&compressed.size()<rle.size(),"negotiated RLE LZ4 wins only for a smaller complete frame payload");
    check(sb::appendCertifiedFrame(bytes(video),descriptor,encoding,bytes(compressed),session.sender,encoded)==sb::FrameStatus::Ok,"compressed mask is signed with its exact original native AU");
    sb::CertifiedAccessUnit result;check(sb::inspectCertifiedFrame(bytes(encoded),session.receiver,result)==sb::FrameStatus::Ok&&result.original==video&&result.coverageEncoding==sb::CoverageEncoding::RleLz4,"compressed native AU MAC and exact video association accepted");
    check(sb::decodeCertifiedCoverage(result,decoded)&&decoded==raw,"all original coverage bytes including fractional alpha reproduced exactly");
    std::vector<std::uint8_t> detached;check(sb::serializeCoverageMetadata(result,detached),"actual full-AU verified compressed certificate can be detached");
    sb::CertifiedAccessUnit local;check(sb::inspectCoverageMetadata(bytes(detached),session.receiver,local)==sb::FrameStatus::Ok&&!local.originalAccessUnitVerified&&sb::decodeCertifiedCoverage(local,decoded)&&decoded==raw,"authenticated local compressed metadata remains exact without inventing AU verification");
    sb::CoverageRange range;std::vector<std::uint8_t> single;
    check(sb::inspectCoverageMetadataDecoded(bytes(detached),session.receiver,local,single,range)==sb::FrameStatus::Ok&&single==raw&&range.minimum==0&&range.maximum==255&&!local.originalAccessUnitVerified,"authenticated compositor path expands fractional compressed coverage once with its exact range and no invented AU authority");
    for(std::size_t n=0;n<detached.size();++n){auto changed=detached;changed[n]^=0x40;sb::CertifiedAccessUnit unchanged;unchanged.descriptor.sourceFrame=901;
        check(sb::inspectCoverageMetadata(bytes(changed),session.receiver,unchanged)!=sb::FrameStatus::Ok&&unchanged.descriptor.sourceFrame==901,"every changed compressed header/hash/payload rejected transactionally");
        single={91};range={71,82};check(sb::inspectCoverageMetadataDecoded(bytes(changed),session.receiver,unchanged,single,range)!=sb::FrameStatus::Ok&&unchanged.descriptor.sourceFrame==901&&single==std::vector<std::uint8_t>{91}&&range.minimum==71&&range.maximum==82,"changed authenticated-decode input cannot publish certificate samples or content range");}
    for(std::size_t n=0;n<encoded.size();++n){sb::CertifiedAccessUnit unchanged;unchanged.descriptor.sourceFrame=902;check(sb::inspectCertifiedFrame({encoded.data(),n},session.receiver,unchanged)!=sb::FrameStatus::Ok&&unchanged.descriptor.sourceFrame==902,"all truncated compressed AUs rejected transactionally");}
    sb::CoverageEncoding legacy=sb::CoverageEncoding::None;std::vector<std::uint8_t> compatible;
    check(sb::encodeBestCoverage(bytes(raw),false,legacy,compatible)&&legacy==sb::CoverageEncoding::Rle&&compatible==rle,"unnegotiated peer receives only an existing encoding with exact unchanged RLE");
    std::vector<std::uint8_t> ramp(256);for(unsigned i=0;i<256;++i)ramp[i]=static_cast<std::uint8_t>(i);
    check(sb::encodeBestCoverage(bytes(ramp),true,encoding,compressed)&&encoding==sb::CoverageEncoding::Raw&&compressed==ramp,"incompressible fractional-alpha ramp uses original raw bytes instead of expansion");
    const std::vector<std::uint8_t> tiny{34};check(sb::encodeBestCoverage(bytes(tiny),true,encoding,compressed)&&encoding==sb::CoverageEncoding::Raw&&compressed==tiny,"one-pixel mask is lossless and never inflated by framing");
    compressed={83};encoding=sb::CoverageEncoding::None;check(!sb::encodeBestCoverage({},true,encoding,compressed)&&encoding==sb::CoverageEncoding::None&&compressed==std::vector<std::uint8_t>{83},"invalid selector input leaves both outputs unchanged");
}
void malformed(){
    Session session;const sb::FrameDescriptor descriptor{9,7,64,32};
    std::vector<std::uint8_t> raw(2048,127),rle,encoded;check(sb::encodeCoverageRle(bytes(raw),rle)&&sb::compressCoverageRle(bytes(rle),encoded),"genuine compact baseline block produced");
    std::vector<std::vector<std::uint8_t>> bad{{},{0},{0,0,0,0},{0,0,0,0,0},{2,0,0,0,0x10,1,0,0},{18,0,0,0,0xf0},{18,0,0,0,0,0,0},{18,0,0,0,0x10,0,0xff,0xff}};
    for(auto n:{0u,1u,2u,17u,4098u,0xffffffffu}){auto changed=encoded;length(changed,n);bad.push_back(std::move(changed));}
    for(auto count:{0u,1u,255u}){std::vector<std::uint8_t> invalidRle(64);for(unsigned n=0;n<32;++n){invalidRle[2*n]=static_cast<std::uint8_t>(count);invalidRle[2*n+1]=127;}std::vector<std::uint8_t> value;check(sb::compressCoverageRle(bytes(invalidRle),value),"compact peer block can encode semantically invalid RLE");bad.push_back(std::move(value));}
    for(std::size_t n=0;n<encoded.size();++n)bad.emplace_back(encoded.begin(),encoded.begin()+static_cast<std::ptrdiff_t>(n));
    for(const auto& value:bad){std::vector<std::uint8_t> unchanged{81};check(!sb::decompressCoverageRle(bytes(value),raw.size(),unchanged)&&unchanged==std::vector<std::uint8_t>{81},"bad prefix/literal/offset/bounds/truncation rejected without publishing pixels");
        const auto metadata=signedMalformed(session,bytes(value),descriptor);sb::CertifiedAccessUnit result;result.descriptor.sourceFrame=903;
        check(sb::inspectCoverageMetadata(bytes(metadata),session.receiver,result)==sb::FrameStatus::Malformed&&result.descriptor.sourceFrame==903,"even a correctly authenticated malformed block is refused transactionally");
        sb::CoverageRange range{21,71};unchanged={81};check(sb::inspectCoverageMetadataDecoded(bytes(metadata),session.receiver,result,unchanged,range)==sb::FrameStatus::Malformed&&result.descriptor.sourceFrame==903&&unchanged==std::vector<std::uint8_t>{81}&&range.minimum==21&&range.maximum==71,"authenticated malformed combined decoder leaves every output unchanged");}
    auto prefixBomb=encoded;length(prefixBomb,0x7ffffffe);auto packet=signedMalformed(session,bytes(prefixBomb),descriptor);packet[sb::FrameSignedHeaderBytes]^=1;sb::CertifiedAccessUnit rejected;
    check(sb::inspectCoverageMetadata(bytes(packet),session.receiver,rejected)==sb::FrameStatus::AuthenticationRefused,"bad MAC is rejected before processing the claimed decompressed allocation size");
    std::vector<std::uint8_t> output{9};check(!sb::decompressCoverageRle(bytes(encoded),0,output)&&!sb::decompressCoverageRle(bytes(encoded),sb::MaximumCoverageBytes+1,output)&&output==std::vector<std::uint8_t>{9},"invalid frame extents cannot allocate a decoder output");
}
void randomized(){
    std::mt19937 random(739);Session session;const auto video=nativeVideo();
    for(unsigned n=0;n<300;++n){const unsigned width=1+random()%129,height=1+random()%67;std::vector<std::uint8_t> raw(width*height);
        for(std::size_t at=0;at<raw.size();){const auto count=std::min<std::size_t>(raw.size()-at,1+random()%(n%3?500:3));const auto value=static_cast<std::uint8_t>(random());std::fill_n(raw.begin()+static_cast<std::ptrdiff_t>(at),count,value);at+=count;}
        sb::CoverageEncoding mode;std::vector<std::uint8_t> selected,restored,encoded;check(sb::encodeBestCoverage(bytes(raw),true,mode,selected)&&selected.size()<=raw.size(),"deterministic random masks never grow on wire");
        sb::CertifiedAccessUnit certificate;check(sb::appendCertifiedFrame(bytes(video),{1+n,3,static_cast<std::uint16_t>(width),static_cast<std::uint16_t>(height)},mode,bytes(selected),session.sender,encoded)==sb::FrameStatus::Ok&&sb::inspectCertifiedFrame(bytes(encoded),session.receiver,certificate)==sb::FrameStatus::Ok&&sb::decodeCertifiedCoverage(certificate,restored)&&restored==raw,"random full-AU signed compressed/raw/RLE samples survive bit exact round trip");
        std::vector<std::uint8_t> detached;sb::CoverageRange range;const auto expectedRange=std::minmax_element(raw.begin(),raw.end());
        check(sb::serializeCoverageMetadata(certificate,detached)&&sb::inspectCoverageMetadataDecoded(bytes(detached),session.receiver,certificate,restored,range)==sb::FrameStatus::Ok&&restored==raw&&range.minimum==*expectedRange.first&&range.maximum==*expectedRange.second,"combined authenticated decoder retains all randomized fractional pixels and exact range across selected encodings");
    }
    for(unsigned n=0;n<20000;++n){std::vector<std::uint8_t> corrupt(4+random()%160);for(auto& value:corrupt)value=static_cast<std::uint8_t>(random());if(n%2)length(corrupt,2*(1+random()%2048));std::vector<std::uint8_t> output{71};
        const auto accepted=sb::decompressCoverageRle(bytes(corrupt),2048,output);check(!accepted||output.size()<=4096,"random bounded malformed blocks never exceed descriptor-derived allocation cap");if(!accepted)check(output==std::vector<std::uint8_t>{71},"random invalid block keeps caller output unchanged");}
}
using Clock=std::chrono::steady_clock;
void benchmark(){
    constexpr unsigned width=1152,height=4608,iterations=20;std::vector<std::uint8_t> raw(width*height);
    for(unsigned kind=0;kind<5;++kind){std::mt19937 random(739);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const bool panel=x>width/6&&x<5*width/6&&y>height/8&&y<7*height/8;
            std::uint8_t value=0;
            if(!kind)value=panel?255:0;
            else if(kind==1)value=static_cast<std::uint8_t>((x/8+3*(y/8))%256);
            else if(kind==2)value=panel?(x%19<9&&y%23<14?255:192):0;
            else if(kind==3)value=static_cast<std::uint8_t>(random());
            else {const auto edge=(y*13+y/7)%351;value=x>edge&&x<width-edge/2?static_cast<std::uint8_t>((y/7)%3?255:127):0;}
            raw[std::size_t(y)*width+x]=value;
        }
        std::vector<std::uint8_t> rle,encoded,decoded;sb::CoverageEncoding mode=sb::CoverageEncoding::None;check(sb::encodeCoverageRle(bytes(raw),rle),"representative RLE baseline available");
        const auto legacyStart=Clock::now();for(unsigned n=0;n<iterations;++n)check(sb::encodeBestCoverage(bytes(raw),false,mode,encoded),"legacy selector succeeds");
        const auto start=Clock::now();
        for(unsigned n=0;n<iterations;++n)check(sb::encodeBestCoverage(bytes(raw),true,mode,encoded),"representative selector succeeds");
        const auto afterEncode=Clock::now();
        sb::CertifiedAccessUnit certificate;certificate.descriptor={1,1,width,height};certificate.coverageEncoding=mode;certificate.coverage=encoded;
        for(unsigned n=0;n<iterations;++n)check(sb::decodeCertifiedCoverage(certificate,decoded)&&decoded==raw,"representative decoded bytes remain identical");
        const auto end=Clock::now();
        const auto legacy=std::chrono::duration<double,std::milli>(start-legacyStart).count()/iterations,enc=std::chrono::duration<double,std::milli>(afterEncode-start).count()/iterations,dec=std::chrono::duration<double,std::milli>(end-afterEncode).count()/iterations;
        std::printf("{\"case\":\"%s\",\"width\":%u,\"height\":%u,\"raw_bytes\":%zu,\"rle_bytes\":%zu,\"selected_encoding\":%u,\"wire_bytes\":%zu,\"ratio_to_rle\":%.6f,\"legacy_encode_ms\":%.3f,\"encode_ms\":%.3f,\"decode_and_compare_ms\":%.3f,\"iterations\":%u}\n",std::array<const char*,5>{{"panel","fractional-grid","text-like","noise","irregular-edges"}}[kind],width,height,raw.size(),rle.size(),unsigned(mode),encoded.size(),double(encoded.size())/double(rle.size()),legacy,enc,dec,iterations);
    }
}
}
int main(int argc,char** argv){if(argc==2&&!std::strcmp(argv[1],"--benchmark"))benchmark();else{roundTrip();malformed();randomized();}std::printf("%u lossless coverage compression checks passed\n",checks);}
