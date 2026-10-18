#include "hevc_bridge_sei.h"
#include <algorithm>

namespace stream_bridge {
namespace {
struct Nal { std::size_t start,body,end,trimmedEnd;unsigned type,layer,temporal;bool owned=false,mixed=false; };
bool startCode(Bytes b,std::size_t from,std::size_t& at,std::size_t& length) {
    for(std::size_t i=from;i+2<b.size;++i)if(!b.data[i]&&!b.data[i+1]) {
        if(b.data[i+2]==1){at=i;length=3;return true;}
        if(i+3<b.size&&!b.data[i+2]&&b.data[i+3]==1){at=i;length=4;return true;}
    }
    return false;
}
SeiStatus unescape(Bytes b,std::vector<std::uint8_t>& out) {
    unsigned zeros=0;
    for(std::size_t i=0;i<b.size;++i) {
        const auto value=b.data[i];
        if(zeros==2) {
            if(value==3){if(i+1==b.size||b.data[i+1]>3)return SeiStatus::Malformed;zeros=0;continue;}
            if(value<3)return SeiStatus::Malformed;
        }
        out.push_back(value);zeros=value==0?std::min(2u,zeros+1):0;
    }
    return SeiStatus::Ok;
}
SeiStatus sei(Bytes au,Nal& n,std::vector<std::uint8_t>& owned) {
    if(n.trimmedEnd-n.body<3)return SeiStatus::Malformed;
    const std::size_t length=n.trimmedEnd-n.body-2;
    if(length>MaximumSeiBytes)return SeiStatus::LimitExceeded;
    std::vector<std::uint8_t> rbsp;rbsp.reserve(length);
    auto status=unescape({au.data+n.body+2,length},rbsp);if(status!=SeiStatus::Ok)return status;
    if(rbsp.empty()||rbsp.back()!=0x80)return SeiStatus::Malformed;
    const auto end=rbsp.size()-1;std::size_t i=0,messages=0;
    while(i<end) {
        std::size_t type=0,size=0;
        for(;;){if(i==end)return SeiStatus::Malformed;const auto x=rbsp[i++];type+=x;if(x!=255)break;}
        for(;;){if(i==end)return SeiStatus::Malformed;const auto x=rbsp[i++];size+=x;if(x!=255)break;}
        if(size>end-i)return SeiStatus::Malformed;
        ++messages;
        if(type==5) {
            if(size<BridgeUuid.size())return SeiStatus::Malformed;
            if(std::equal(BridgeUuid.begin(),BridgeUuid.end(),rbsp.begin()+static_cast<std::ptrdiff_t>(i))) {
                if(n.owned)return SeiStatus::DuplicateOwned;
                const auto payloadBytes=size-BridgeUuid.size();
                if(!payloadBytes||payloadBytes>MaximumOwnedPayloadBytes)return SeiStatus::LimitExceeded;
                n.owned=true;owned.assign(rbsp.begin()+static_cast<std::ptrdiff_t>(i+BridgeUuid.size()),rbsp.begin()+static_cast<std::ptrdiff_t>(i+size));
            }
        }
        i+=size;
    }
    if(!messages)return SeiStatus::Malformed;
    n.mixed=n.owned&&messages!=1;
    return n.mixed?SeiStatus::MixedOwned:SeiStatus::Ok;
}
SeiStatus parse(Bytes au,std::vector<Nal>& nals,std::vector<std::uint8_t>& owned) {
    if(!au.data||!au.size)return SeiStatus::InvalidArgument;
    if(au.size>MaximumAccessUnitBytes)return SeiStatus::LimitExceeded;
    std::size_t at=0,prefix=0;
    if(!startCode(au,0,at,prefix))return SeiStatus::Malformed;
    for(std::size_t i=0;i<at;++i)if(au.data[i])return SeiStatus::Malformed;
    unsigned found=0;
    for(;;) {
        if(nals.size()>=MaximumNalCount)return SeiStatus::LimitExceeded;
        const auto body=at+prefix;
        std::size_t next=0,nextPrefix=0;
        const bool another=startCode(au,body,next,nextPrefix);
        const auto end=another?next:au.size;
        if(end-body<2)return SeiStatus::Malformed;
        const auto h0=au.data[body],h1=au.data[body+1];
        if((h0&0x80)||!(h1&7))return SeiStatus::Malformed;
        auto trimmed=end;while(trimmed>body+2&&!au.data[trimmed-1])--trimmed;
        Nal n{at,body,end,trimmed,unsigned((h0>>1)&63),unsigned(((h0&1)<<5)|(h1>>3)),unsigned(h1&7)};
        if(n.type==39||n.type==40) {
            auto status=sei(au,n,owned);if(status!=SeiStatus::Ok)return status;
            if(n.owned&&++found>1)return SeiStatus::DuplicateOwned;
        }
        nals.push_back(n);
        if(!another)break;
        at=next;prefix=nextPrefix;
    }
    return SeiStatus::Ok;
}
void number(std::vector<std::uint8_t>& b,std::size_t value){while(value>=255){b.push_back(255);value-=255;}b.push_back(static_cast<std::uint8_t>(value));}
std::vector<std::uint8_t> encoded(Bytes payload,unsigned layer,unsigned temporal) {
    std::vector<std::uint8_t> rbsp;number(rbsp,5);number(rbsp,BridgeUuid.size()+payload.size);
    rbsp.insert(rbsp.end(),BridgeUuid.begin(),BridgeUuid.end());rbsp.insert(rbsp.end(),payload.data,payload.data+payload.size);rbsp.push_back(0x80);
    std::vector<std::uint8_t> result{0,0,0,1,static_cast<std::uint8_t>((39<<1)|(layer>>5)),static_cast<std::uint8_t>((layer<<3)|temporal)};
    unsigned zeros=0;
    for(auto value:rbsp){if(zeros==2&&value<=3){result.push_back(3);zeros=0;}result.push_back(value);zeros=value==0?std::min(2u,zeros+1):0;}
    return result;
}
}
SeiStatus inspectOwned(Bytes au,std::vector<std::uint8_t>& output) {
    std::vector<Nal> nals;std::vector<std::uint8_t> payload;
    const auto status=parse(au,nals,payload);if(status!=SeiStatus::Ok)return status;
    if(payload.empty())return SeiStatus::NotFound;
    output.swap(payload);return SeiStatus::Ok;
}
SeiStatus appendOwned(Bytes au,Bytes payload,std::vector<std::uint8_t>& output) {
    if(!payload.data||!payload.size)return SeiStatus::InvalidArgument;
    if(payload.size>MaximumOwnedPayloadBytes)return SeiStatus::LimitExceeded;
    std::vector<Nal> nals;std::vector<std::uint8_t> existing;
    const auto status=parse(au,nals,existing);if(status!=SeiStatus::Ok)return status;
    if(!existing.empty())return SeiStatus::DuplicateOwned;
    const auto first=std::find_if(nals.begin(),nals.end(),[](const Nal& n){return n.type<=31;});
    if(first==nals.end())return SeiStatus::NoVcl;
    const auto added=encoded(payload,first->layer,first->temporal);
    if(added.size()>MaximumAccessUnitBytes-au.size)return SeiStatus::LimitExceeded;
    std::vector<std::uint8_t> result;result.reserve(au.size+added.size());
    result.insert(result.end(),au.data,au.data+first->start);result.insert(result.end(),added.begin(),added.end());result.insert(result.end(),au.data+first->start,au.data+au.size);
    output.swap(result);return SeiStatus::Ok;
}
SeiStatus removeOwned(Bytes au,std::vector<std::uint8_t>& output) {
    std::vector<Nal> nals;std::vector<std::uint8_t> existing;
    const auto status=parse(au,nals,existing);if(status!=SeiStatus::Ok)return status;
    const auto found=std::find_if(nals.begin(),nals.end(),[](const Nal& n){return n.owned;});
    if(found==nals.end())return SeiStatus::NotFound;
    if(found->mixed)return SeiStatus::MixedOwned;
    std::vector<std::uint8_t> result;result.reserve(au.size-(found->trimmedEnd-found->start));
    result.insert(result.end(),au.data,au.data+found->start);result.insert(result.end(),au.data+found->trimmedEnd,au.data+au.size);
    output.swap(result);return SeiStatus::Ok;
}
} // namespace stream_bridge
