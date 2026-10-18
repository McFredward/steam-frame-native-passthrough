#include "bridge/stream_bridge_frame.h"
#include "bridge/hevc_bridge_sei.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace sb=stream_bridge;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
sb::Bytes bytes(const std::vector<std::uint8_t>& b){return {b.data(),b.size()};}
struct Sessions{
    sb::Pairing producer{},receiver{};sb::Nonce binding{},connection{};sb::AuthSession sender,observer;
    Sessions(){producer.role=sb::Role::Producer;receiver.role=sb::Role::Receiver;
        for(unsigned i=0;i<16;++i){producer.pairId[i]=receiver.pairId[i]=std::uint8_t(10+i);binding[i]=std::uint8_t(40+i);connection[i]=std::uint8_t(70+i);}
        for(unsigned i=0;i<32;++i)producer.key[i]=receiver.key[i]=std::uint8_t(100+i);
        check(sender.prepareProducer(producer,binding)==sb::AuthStatus::Ok,"real producer auth prepared");
        check(observer.prepareReceiver(receiver,{connection,1})==sb::AuthStatus::Ok,"real receiver auth prepared");
    }
};
std::vector<std::uint8_t> video(){
    // Exact owned test bytes; actual codec decoding is a separate file mode.
    return {0,0,0,1,0x40,1,0x91,0x80,0,0,1,0x02,1,0x80,0x42,0x80,0,0};
}
void run(){
    Sessions s;const auto original=video();const sb::FrameDescriptor descriptor{9,3,64,32};
    std::vector<std::uint8_t> held{99};
    check(sb::appendCertifiedFrame(bytes(original),descriptor,sb::CoverageEncoding::None,{},s.sender,held)==sb::FrameStatus::Ok,"certificate inserted in original stream");
    sb::CertifiedAccessUnit result;
    check(sb::inspectCertifiedFrame(bytes(held),s.observer,result)==sb::FrameStatus::Ok,"actual MAC and exact original hash accepted");
    check(result.original==original&&result.coverage.empty()&&result.binding==s.binding,"all original bytes including padding restored exactly");
    check(result.descriptor.sourceFrame==9&&result.descriptor.encoderGeneration==3&&result.descriptor.codedWidth==64&&result.descriptor.codedHeight==32,"separate source identity and coded extent retained");
    check(!s.observer.bound()&&!s.observer.peerVerified(),"certificate never bootstraps or confirms a peer");
    std::vector<std::uint8_t> output{37};check(!sb::decodeCertifiedCoverage(result,output)&&output==std::vector<std::uint8_t>{37},"byte-only proof cannot become a mask");
    auto duplicate=held;check(sb::appendCertifiedFrame(bytes(held),descriptor,sb::CoverageEncoding::None,{},s.sender,duplicate)==sb::FrameStatus::DuplicateOwned&&duplicate==held,"duplicate owned certificate refuses transactionally");
    const auto stable=held;
    std::vector<std::uint8_t> wide;
    check(sb::appendCertifiedFrame(bytes(original),{1,1,3264,6400},sb::CoverageEncoding::None,{},s.sender,wide)==sb::FrameStatus::Ok,"retained real coded extent is not rejected by mask budget");
    for(std::size_t cut=0;cut<stable.size();++cut){sb::CertifiedAccessUnit unchanged;unchanged.descriptor.sourceFrame=555;
        check(sb::inspectCertifiedFrame({stable.data(),cut},s.observer,unchanged)!=sb::FrameStatus::Ok&&unchanged.descriptor.sourceFrame==555,"every truncated stream fails without publishing proof");}
    for(std::size_t at=0;at<stable.size();++at){auto corrupt=stable;corrupt[at]^=0x40;sb::CertifiedAccessUnit unchanged;unchanged.descriptor.sourceFrame=556;
        const auto status=sb::inspectCertifiedFrame(bytes(corrupt),s.observer,unchanged);
        if(status==sb::FrameStatus::Ok)std::fprintf(stderr,"Accepted mutation at byte %zu\n",at);
        check(status!=sb::FrameStatus::Ok&&unchanged.descriptor.sourceFrame==556,"changed source/certificate/payload fails transactionally");}
    auto extra=stable;extra.insert(extra.end(),{0,0,1,0x02,1,0x80});
    check(sb::inspectCertifiedFrame(bytes(extra),s.observer,result)!=sb::FrameStatus::Ok,"aggregated additional VCL not the original complete AU");
    auto second=stable;second.insert(second.end(),stable.begin(),stable.end());
    check(sb::inspectCertifiedFrame(bytes(second),s.observer,result)==sb::FrameStatus::DuplicateOwned,"two certificates are ambiguous");
    auto wrong=s.receiver;wrong.key[7]^=1;sb::AuthSession other;check(other.prepareReceiver(wrong,{s.connection,2})==sb::AuthStatus::Ok,"wrong-key peer exists");
    check(sb::inspectCertifiedFrame(bytes(stable),other,result)==sb::FrameStatus::AuthenticationRefused,"wrong key cannot bless complete bytes");
    sb::Nonce newer=s.binding;newer[0]^=0x20;sb::AuthSession newerProducer;check(newerProducer.prepareProducer(s.producer,newer)==sb::AuthStatus::Ok,"another source binding exists");
    std::vector<std::uint8_t> newerFrame;check(sb::appendCertifiedFrame(bytes(original),descriptor,sb::CoverageEncoding::None,{},newerProducer,newerFrame)==sb::FrameStatus::Ok,"new binding signed independently");
    sb::LocalConfig config{sb::Role::Producer,123,s.connection};sb::Handshake handshake;sb::VerifiedChannel channel{true,s.binding};
    check(handshake.begin(config,channel)==sb::Status::Ok,"producer HELLO prepared");sb::Message hello;check(handshake.next(hello)==sb::Status::Ok,"real HELLO available");
    std::array<std::uint8_t,sb::MessageBytes> raw{};check(sb::encode(hello,raw)==sb::Status::Ok,"HELLO encoded");sb::AuthEnvelope envelope{};
    check(s.sender.seal({raw.data(),raw.size()},envelope)==sb::AuthStatus::Ok,"HELLO authenticated");
    check(s.observer.verifyInitialVideoHello({envelope.data(),envelope.size()},s.binding)==sb::AuthStatus::Ok&&!s.observer.bound(),"preflight validates matching HELLO but no authority is consumed");
    check(s.observer.verifyInitialVideoHello({envelope.data(),envelope.size()},newer)!=sb::AuthStatus::Ok,"certificate and HELLO bindings must match");
    sb::AuthPayload body{};sb::VerifiedChannel bound{};
    check(s.observer.bootstrapFromVideo({envelope.data(),envelope.size()},{{s.connection,1},true},body,bound)==sb::AuthStatus::Ok,"actual existing bootstrap independently consumes HELLO");
    check(sb::inspectCertifiedFrame(bytes(newerFrame),s.observer,result)==sb::FrameStatus::AuthenticationRefused,"bound peer refuses a different certificate binding");
    std::vector<std::uint8_t> rawCoverage(64*32);for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x)rawCoverage[y*64+x]=x<16?0:x<32?64:x<48?127:255;
    std::vector<std::uint8_t> rle;check(sb::encodeCoverageRle(bytes(rawCoverage),rle)&&rle.size()<rawCoverage.size(),"fractional real-valued coverage compressed losslessly");
    check(sb::appendCertifiedFrame(bytes(original),descriptor,sb::CoverageEncoding::Rle,bytes(rle),s.sender,held)==sb::FrameStatus::Ok,"RLE bound to same exact AU");
    check(sb::inspectCertifiedFrame(bytes(held),s.observer,result)==sb::FrameStatus::Ok,"RLE digest and MAC accepted");
    check(sb::decodeCertifiedCoverage(result,output)&&output==rawCoverage,"all 2048 coverage samples reproduced exactly");
    std::vector<std::uint8_t> detached;
    check(result.originalAccessUnitVerified&&sb::serializeCoverageMetadata(result,detached),"actual native-AU-verified result exports exact signed mask metadata");
    sb::CertifiedAccessUnit local;
    check(sb::inspectCoverageMetadata(bytes(detached),s.observer,local)==sb::FrameStatus::Ok&&local.original.empty()&&!local.originalAccessUnitVerified,"local mask verification never manufactures video completeness");
    check(sb::decodeCertifiedCoverage(local,output)&&output==rawCoverage,"local detached mask samples exact");
    check(!sb::serializeCoverageMetadata(local,held),"detached local mask cannot be promoted to full source proof");
    for(std::size_t at=0;at<detached.size();++at){auto bad=detached;bad[at]^=0x40;sb::CertifiedAccessUnit unchanged;unchanged.descriptor.sourceFrame=557;
        check(sb::inspectCoverageMetadata(bytes(bad),s.observer,unchanged)!=sb::FrameStatus::Ok&&unchanged.descriptor.sourceFrame==557,"local metadata tampering refused transactionally");}

    check(sb::appendCertifiedFrame(bytes(original),descriptor,sb::CoverageEncoding::Raw,bytes(rawCoverage),s.sender,held)==sb::FrameStatus::Ok,"uncompressed coverage accepted");
    check(sb::inspectCertifiedFrame(bytes(held),s.observer,result)==sb::FrameStatus::Ok&&sb::decodeCertifiedCoverage(result,output)&&output==rawCoverage,"raw coverage exact");
    for(auto malformed:std::vector<std::vector<std::uint8_t>>{{0,5},{255,0,255},{255,1},{1,1,1,2}}){output={17};
        check(sb::appendCertifiedFrame(bytes(original),descriptor,sb::CoverageEncoding::Rle,bytes(malformed),s.sender,output)==sb::FrameStatus::InvalidArgument&&output==std::vector<std::uint8_t>{17},"invalid RLE never publishes a frame");}
    for(auto d:std::vector<sb::FrameDescriptor>{{0,1,64,32},{1,0,64,32},{1,1,0,32},{1,1,8193,8192}}){output={18};
        check(sb::appendCertifiedFrame(bytes(original),d,sb::CoverageEncoding::None,{},s.sender,output)==sb::FrameStatus::InvalidArgument&&output==std::vector<std::uint8_t>{18},"missing or unbounded identity/extent refused");}
    std::vector<std::uint8_t> withHello;check(sb::appendOwned(bytes(original),{envelope.data(),envelope.size()},withHello)==sb::SeiStatus::Ok,"original discovery SEI inserted by actual old codec");
    check(sb::appendCertifiedFrame(bytes(withHello),descriptor,sb::CoverageEncoding::None,{},s.sender,held)==sb::FrameStatus::Ok,"certificate follows control augmentation");
    check(sb::inspectCertifiedFrame(bytes(held),s.observer,result)==sb::FrameStatus::Ok&&result.original==withHello,"certificate removal preserves entire control SEI and native bytes");
}
void fileMode(const char* input,const char* encoded,const char* restored){
    std::ifstream stream(input,std::ios::binary);std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(stream)),{});check(!original.empty(),"actual encoded HEVC read");
    Sessions s;sb::FrameDescriptor descriptor{1,1,64,64};std::vector<std::uint8_t> output;
    check(sb::appendCertifiedFrame(bytes(original),descriptor,sb::CoverageEncoding::None,{},s.sender,output)==sb::FrameStatus::Ok,"actual HEVC certificate append");
    sb::CertifiedAccessUnit result;check(sb::inspectCertifiedFrame(bytes(output),s.observer,result)==sb::FrameStatus::Ok&&result.original==original,"actual HEVC exact round trip");
    std::ofstream a(encoded,std::ios::binary),b(restored,std::ios::binary);a.write(reinterpret_cast<const char*>(output.data()),output.size());b.write(reinterpret_cast<const char*>(result.original.data()),result.original.size());
}
}
int main(int argc,char** argv){if(argc==4)fileMode(argv[1],argv[2],argv[3]);else run();std::printf("%u exact-AU certificate checks passed\n",checks);}
