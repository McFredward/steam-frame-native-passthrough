#include "bridge/stream_bridge_frame.h"
#include "bridge/stream_bridge_compression.h"
#include "pc/stream-bridge-source-contract.h"
#include "pc/stream-bridge-mask-content.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace sb=stream_bridge;
namespace {
unsigned checks=0;
void check(bool value,const char* text){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
sb::Bytes bytes(const std::vector<std::uint8_t>& v){return {v.data(),v.size()};}
std::vector<std::uint8_t> pattern(unsigned width,unsigned height,unsigned kind){
    std::vector<std::uint8_t> raw(std::size_t(width)*height);std::mt19937 random(739);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const bool panel=x>width/6&&x<5*width/6&&y>height/8&&y<7*height/8;unsigned value=0;
        if(kind==0)value=panel?255:0;
        else if(kind==1)value=(x/8+3*(y/8))%256;
        else if(kind==2)value=panel?(x%19<9&&y%23<14?255:192):0;
        else if(kind==3)value=random()%256;
        else if(kind==4)value=247+random()%9;
        else if(kind==5)value=panel?(y%79<61&&x%43<20?(160+x/20+y/30)%256:255):0;
        else value=panel?(random()%16==0?247+random()%9:255):0;
        raw[std::size_t(y)*width+x]=static_cast<std::uint8_t>(value);
    }return raw;
}
void run(){
    constexpr std::uint16_t width=256,height=256;const auto raw=pattern(width,height,4);
    sb::CoverageEncoding oldMode,newMode;std::vector<std::uint8_t> old,encoded,again;
    check(sb::encodeBestCoverage(bytes(raw),true,oldMode,old),"original lossless selector accepts fractional mask");
    check(sb::encodeBestCoverage(bytes(raw),true,newMode,encoded,true)&&newMode==sb::CoverageEncoding::RleZstd&&encoded.size()<old.size(),"opt-in entropy encoding strictly beats current full payload on noisy fractional alpha");
    check(sb::encodeBestCoverage(bytes(raw),true,newMode,again,false)&&newMode==oldMode&&again==old,"unnegotiated entropy retains byte-for-byte legacy compression");
    sb::CertifiedAccessUnit frame;frame.descriptor={1,1,width,height};frame.coverageEncoding=sb::CoverageEncoding::RleZstd;frame.coverage=encoded;
    std::vector<std::uint8_t> decoded;sb::CoverageRange range;
    check(sb::decodeCertifiedCoverage(frame,decoded,range)&&decoded==raw&&range.minimum==247&&range.maximum==255,"every fractional sample is reconstructed exactly");
    std::vector<std::uint8_t> rle;check(sb::decompressCoverageRleZstd(bytes(encoded),raw.size(),rle),"bounded RLE expansion accepts the exact standalone frame");
    auto invalid=encoded;invalid.push_back(0);std::vector<std::uint8_t> untouched{91};
    check(!sb::decompressCoverageRleZstd(bytes(invalid),raw.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"trailing data never publishes pixels");
    invalid=encoded;invalid.insert(invalid.end(),encoded.begin()+4,encoded.end());
    check(!sb::decompressCoverageRleZstd(bytes(invalid),raw.size(),untouched),"concatenated compressed frames are rejected");
    for(unsigned n:{0u,1u,2u,3u,0xffffffffu}){invalid=encoded;for(unsigned i=0;i<4;++i)invalid[i]=static_cast<std::uint8_t>(n>>(8*i));
        check(!sb::decompressCoverageRleZstd(bytes(invalid),raw.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"invalid decoded lengths are rejected transactionally");}
    for(std::size_t cut=0;cut<encoded.size();cut+=std::max<std::size_t>(1,encoded.size()/128))
        check(!sb::decompressCoverageRleZstd({encoded.data(),cut},raw.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"truncated blocks cannot publish partial coverage");
    std::mt19937 random(829);for(unsigned n=0;n<10000;++n){std::vector<std::uint8_t> malformed(4+random()%100);for(auto& v:malformed)v=static_cast<std::uint8_t>(random());
        const bool accepted=sb::decompressCoverageRleZstd(bytes(malformed),raw.size(),untouched);
        check(!accepted&&untouched==std::vector<std::uint8_t>{91},"bounded malformed-frame controls preserve caller output");}
    auto overBudget=pattern(1024,1024,3);std::vector<std::uint8_t> largeRle,baseline,limited;
    check(sb::encodeCoverageRle(bytes(overBudget),largeRle)&&largeRle.size()>sb::MaximumEntropyRleBytes,"large noisy masks exceed the bounded additional compression budget");
    check(!sb::compressCoverageRleZstd(bytes(largeRle),largeRle.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"oversized RLE cannot enter the optional compressor");
    check(sb::encodeBestCoverage(bytes(overBudget),true,oldMode,baseline)&&sb::encodeBestCoverage(bytes(overBudget),true,newMode,limited,true)&&newMode==oldMode&&limited==baseline,"large masks retain the exact original selector without extra entropy work");
    invalid=encoded;const auto oversized=sb::MaximumEntropyRleBytes+2;for(unsigned i=0;i<4;++i)invalid[i]=static_cast<std::uint8_t>(oversized>>(8*i));
    check(!sb::decompressCoverageRleZstd(bytes(invalid),overBudget.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"receiver rejects entropy RLE beyond the CPU ceiling before allocation");
    const auto detailed=pattern(1024,1024,4);
    check(sb::encodeBestCoverage(bytes(detailed),true,oldMode,baseline)&&sb::encodeBestCoverage(bytes(detailed),true,newMode,limited,true,true)
        &&newMode==sb::CoverageEncoding::RawZstd&&limited.size()<baseline.size(),"detailed masks can use the separately negotiated fast direct-byte codec without a large RLE expansion");
    frame.descriptor={3,1,1024,1024};frame.coverageEncoding=newMode;frame.coverage=limited;
    check(sb::decodeCertifiedCoverage(frame,decoded)&&decoded==detailed,"fast direct-mask mode preserves all opacity samples exactly");
    invalid=limited;invalid.push_back(0);check(!sb::decompressCoverageRawZstd(bytes(invalid),detailed.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"raw entropy rejects trailing input transactionally");
    check(!sb::decompressCoverageRawZstd(bytes(limited),detailed.size()-1,untouched)&&untouched==std::vector<std::uint8_t>{91},"raw decoded extent must exactly match the authenticated descriptor");
    for(unsigned n=0;n<10000;++n){std::vector<std::uint8_t> malformed(4+random()%100);for(auto& v:malformed)v=static_cast<std::uint8_t>(random());
        check(!sb::decompressCoverageRawZstd(bytes(malformed),detailed.size(),untouched)&&untouched==std::vector<std::uint8_t>{91},"raw malformed-frame controls never publish partial output");}
    sb::sender::SourceContract source;source.publish(73);const auto v1=source.snapshot(73);
    check(source.entropyCodecAllowed(v1,73,true),"every live explicit-RGBA source can use mutually negotiated lossless encoding");
    source.publish(73,true);const auto v2=source.snapshot(73);
    check(source.entropyCodecAllowed(v2,73,true)&&!source.entropyCodecAllowed(v2,73,false),"v2 opt-in requires both peer capability and live application policy");
    check(!source.entropyCodecAllowed(0,73,true)&&!source.entropyCodecAllowed(v2,74,true),"absent/foreign declarations cannot select the codec");
    source.publish(0);check(!source.entropyCodecAllowed(v2,73,true),"source loss revokes optimized encoding eligibility");
    source.publish(73);check(source.entropyCodecAllowed(source.snapshot(73),73,true),"source version is independent of optional lossless codec support");
    sb::sender::MaskContentSample stats{};check(sb::sender::inspectMaskContent(bytes(raw),width,height,stats)&&stats.samples<=32768&&stats.samples==stats.scene+stats.real+stats.fractional&&stats.distinct==9,"bounded statistics reveal fractional-mask complexity without changing pixels");
    sb::sender::MaskContentPublication publication;check(!publication.read(stats),"no diagnostic publication is not a sample");
    check(publication.due(),"first v2 mask can provide a bounded diagnostic sample");publication.publish(stats);sb::sender::MaskContentSample copy;
    check(publication.read(copy)&&copy.samples==stats.samples&&copy.fractional==stats.fractional&&copy.sequence,"diagnostic reader receives one coherent statistical sample");
    for(unsigned i=0;i<127;++i)check(!publication.due(),"diagnostic full-mask sampling is not performed on every frame");
    check(publication.due(),"next diagnostic sample occurs after the bounded cadence");
    for(unsigned kind=0;kind<7;++kind){auto p=pattern(96,80,kind);std::vector<std::uint8_t> legacy,modern;sb::CoverageEncoding a,b;
        check(sb::encodeBestCoverage(bytes(p),true,a,legacy)&&sb::encodeBestCoverage(bytes(p),true,b,modern,true)&&modern.size()<=legacy.size(),"opt-in representation never exceeds the legacy complete payload");
        frame.descriptor={2,1,96,80};frame.coverageEncoding=b;frame.coverage=modern;
        check(sb::decodeCertifiedCoverage(frame,decoded)&&decoded==p,"all tested spatial/noisy patterns retain every original byte");}

}
}

void benchmark(){
    constexpr unsigned width=1152,height=4608,iterations=5;
    for(unsigned kind=0;kind<7;++kind){auto raw=pattern(width,height,kind);std::vector<std::uint8_t> legacy,modern,decoded;sb::CoverageEncoding a,b;
        const auto t0=std::chrono::steady_clock::now();for(unsigned i=0;i<iterations;++i)check(sb::encodeBestCoverage(bytes(raw),true,a,legacy),"legacy benchmark encode");
        const auto t1=std::chrono::steady_clock::now();for(unsigned i=0;i<iterations;++i)check(sb::encodeBestCoverage(bytes(raw),true,b,modern,true,true),"optimized benchmark encode");
        const auto t2=std::chrono::steady_clock::now();sb::CertifiedAccessUnit frame;frame.descriptor={1,1,width,height};frame.coverageEncoding=b;frame.coverage=modern;
        for(unsigned i=0;i<iterations;++i)check(sb::decodeCertifiedCoverage(frame,decoded)&&decoded==raw,"actual benchmark decode is byte exact");
        const auto t3=std::chrono::steady_clock::now();
        std::printf("{\"case\":%u,\"pixels\":%zu,\"legacy_bytes\":%zu,\"new_bytes\":%zu,\"encoding\":%u,\"legacy_ms\":%.3f,\"new_ms\":%.3f,\"decode_compare_ms\":%.3f}\n",kind,raw.size(),legacy.size(),modern.size(),unsigned(b),std::chrono::duration<double,std::milli>(t1-t0).count()/iterations,std::chrono::duration<double,std::milli>(t2-t1).count()/iterations,std::chrono::duration<double,std::milli>(t3-t2).count()/iterations);
    }
}
int main(int argc,char**argv){if(argc==2&&!std::strcmp(argv[1],"--benchmark"))benchmark();else run();std::printf("%u entropy codec and content checks passed\n",checks);}
