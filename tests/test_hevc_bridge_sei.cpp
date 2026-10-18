#include "../bridge/hevc_bridge_sei.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace stream_bridge;
namespace {
unsigned checks=0;
void require(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
using V=std::vector<std::uint8_t>;
Bytes view(const V& b){return {b.data(),b.size()};}
V hex(const std::string& s){V b;std::string d;for(char c:s)if(c!=' ')d+=c;for(std::size_t i=0;i<d.size();i+=2)b.push_back(static_cast<std::uint8_t>(std::stoul(d.substr(i,2),nullptr,16)));return b;}
V concat(const V& a,const V& b){V out=a;out.insert(out.end(),b.begin(),b.end());return out;}
const V vcl=hex("00 00 01 26 01 80 99");
const V payload{0x12,0x34};
const V golden=hex("00 00 00 01 4e 01 05 12 53 46 4e 50 2d 42 52 49 44 47 45 2d 56 31 00 01 12 34 80");
void goldens(){
    V output{0xaa},found{0xbb};require(appendOwned(view(vcl),view(payload),output)==SeiStatus::Ok,"append small payload");require(output==concat(golden,vcl),"independent hand-checked prefix SEI golden");require(inspectOwned(view(output),found)==SeiStatus::Ok&&found==payload,"inspect golden payload");V restored;require(removeOwned(view(output),restored)==SeiStatus::Ok&&restored==vcl,"remove preserves original VCL and three-byte start code");
    V bytes=hex("00 00 00 01 00 00 02 00 00 03 00 00 04");require(appendOwned(view(vcl),view(bytes),output)==SeiStatus::Ok,"append escaped payload");const V escaped=hex("00 00 03 00 01 00 00 03 02 00 00 03 03 00 00 04");require(std::search(output.begin(),output.end(),escaped.begin(),escaped.end())!=output.end(),"hand-checked emulation prevention bytes");require(inspectOwned(view(output),found)==SeiStatus::Ok&&found==bytes,"unescape exact payload");
    V large(256);for(unsigned i=0;i<256;++i)large[i]=static_cast<std::uint8_t>(i);require(appendOwned(view(vcl),view(large),output)==SeiStatus::Ok,"max own payload");require(output[6]==5&&output[7]==255&&output[8]==17,"independent 272-byte payload size extension");require(inspectOwned(view(output),found)==SeiStatus::Ok&&found==large,"size-extension decode");
}
void originalBytes(){
    const V leading=hex("00 00 00 00 01 46 01 80 00 00 01 40 01 80 00 00 00 01 42 01 80 00 00 01 44 01 80");
    const V foreign=hex("00 00 00 01 4e 01 05 11 44 44 44 44 44 44 44 44 44 44 44 44 44 44 44 44 7a 80");
    const V end=hex("00 00 01 4c 01 80 00 00"); // nonowned filler bytes, retained verbatim
    const V original=concat(concat(concat(leading,foreign),vcl),end);V output,restored,p;
    require(inspectOwned(view(original),p)==SeiStatus::NotFound,"foreign UUID never owned");require(appendOwned(view(original),view(payload),output)==SeiStatus::Ok,"append after AUD parameter sets and foreign SEI");require(std::equal(leading.begin(),leading.end(),output.begin()),"AUD VPS SPS PPS byte-identical");require(std::search(output.begin(),output.end(),foreign.begin(),foreign.end())!=output.end(),"foreign SEI verbatim");require(removeOwned(view(output),restored)==SeiStatus::Ok&&restored==original,"whole AU exact restoration including leading trailing zeros");
    const auto saved=output;require(appendOwned(view(output),view(payload),output)==SeiStatus::DuplicateOwned&&output==saved,"no duplicate or partial AU mutation");require(removeOwned(view(output),output)==SeiStatus::Ok&&output==original,"aliased removal safe");require(appendOwned(view(output),view(payload),output)==SeiStatus::Ok,"aliased append safe");
    V layer=vcl;layer[3]=static_cast<std::uint8_t>((19<<1)|1);layer[4]=static_cast<std::uint8_t>((3<<3)|2); // layer35, temporal_id_plus1=2
    require(appendOwned(view(layer),view(payload),output)==SeiStatus::Ok&&output[4]==0x4f&&output[5]==0x1a,"new SEI inherits actual first VCL layer and temporal ID");
}
void refusal(){
    V output{0xde,0xad},p{0xbe,0xef};const V saved=output,psaved=p;
    auto check=[&](const V& bad,SeiStatus expected){require(inspectOwned(view(bad),p)==expected&&p==psaved,"inspect refusal transactional");require(appendOwned(view(bad),view(payload),output)==expected&&output==saved,"append refusal transactional");require(removeOwned(view(bad),output)==expected&&output==saved,"remove refusal transactional");};
    check(hex("12 00 00 01 26 01 80"),SeiStatus::Malformed);
    check(hex("00 00 01 26"),SeiStatus::Malformed);
    check(hex("00 00 01 a6 01 80"),SeiStatus::Malformed);
    check(hex("00 00 01 26 00 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 05 04 12 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 ff 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 05 ff 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 05 10 00 00 02 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 05 10 00 00 03 04 80"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 05 10 00 00 03"),SeiStatus::Malformed);
    check(hex("00 00 01 4e 01 05 10 53 46 4e 50 2d 42 52 49 44 47 45 2d 56 31 00 01 80"),SeiStatus::LimitExceeded);
    require(inspectOwned({nullptr,3},p)==SeiStatus::InvalidArgument&&p==psaved,"null AU rejected");require(appendOwned(view(vcl),{nullptr,2},output)==SeiStatus::InvalidArgument&&output==saved,"null payload rejected");require(appendOwned(view(vcl),{payload.data(),257},output)==SeiStatus::LimitExceeded&&output==saved,"oversized own payload before read");require(inspectOwned({payload.data(),MaximumAccessUnitBytes+1},p)==SeiStatus::LimitExceeded&&p==psaved,"oversized AU before read");
    V noVcl=hex("00 00 01 40 01 80");require(appendOwned(view(noVcl),view(payload),output)==SeiStatus::NoVcl&&output==saved,"parameter sets alone not coded-AU insertion proof");
    auto duplicate=concat(concat(golden,golden),vcl);check(duplicate,SeiStatus::DuplicateOwned);
    auto mixed=golden;mixed.pop_back();const auto foreign=hex("05 11 44 44 44 44 44 44 44 44 44 44 44 44 44 44 44 44 7a 80");mixed=concat(concat(mixed,foreign),vcl);check(mixed,SeiStatus::MixedOwned);
    V tooMany;for(std::size_t i=0;i<=MaximumNalCount;++i)tooMany=concat(tooMany,vcl);check(tooMany,SeiStatus::LimitExceeded);
    V tooBigSei=hex("00 00 01 4e 01");tooBigSei.insert(tooBigSei.end(),MaximumSeiBytes+1,0x44);tooBigSei.push_back(0x80);tooBigSei=concat(tooBigSei,vcl);check(tooBigSei,SeiStatus::LimitExceeded);
}
}
int main(){try{goldens();originalBytes();refusal();std::cout<<checks<<" HEVC bridge checks passed\n";return EXIT_SUCCESS;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}}
