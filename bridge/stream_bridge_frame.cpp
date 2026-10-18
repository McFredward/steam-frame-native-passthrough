#include "stream_bridge_frame.h"
#include "hevc_bridge_sei.h"
#include "../receiver/sha256.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace stream_bridge {
namespace {
using Digest=std::array<std::uint8_t,32>;
struct Nal{std::size_t start=0,body=0,end=0;unsigned type=0,layer=0,temporal=0;};
void put(std::uint8_t* p,std::uint64_t n,unsigned count){for(unsigned i=0;i<count;++i)p[i]=std::uint8_t(n>>(8*i));}
std::uint64_t get(const std::uint8_t* p,unsigned count){std::uint64_t n=0;for(unsigned i=0;i<count;++i)n|=std::uint64_t(p[i])<<(8*i);return n;}
Digest hash(Bytes b){frame_receiver::Sha256 h;if(b.size)h.update(b.data,b.size);return h.finish();}
bool equal(Bytes a,Bytes b){return a.size==b.size&&std::equal(a.data,a.data+a.size,b.data);}
bool startCode(Bytes b,std::size_t from,std::size_t& at,std::size_t& length){
    for(std::size_t i=from;i+2<b.size;++i)if(!b.data[i]&&!b.data[i+1]){
        if(b.data[i+2]==1){at=i;length=3;return true;}
        if(i+3<b.size&&!b.data[i+2]&&b.data[i+3]==1){at=i;length=4;return true;}
    }return false;
}
FrameStatus nals(Bytes b,std::vector<Nal>& out){
    if(!b.data||!b.size)return FrameStatus::InvalidArgument;
    if(b.size>MaximumAccessUnitBytes)return FrameStatus::LimitExceeded;
    std::size_t at=0,prefix=0;if(!startCode(b,0,at,prefix))return FrameStatus::Malformed;
    for(std::size_t i=0;i<at;++i)if(b.data[i])return FrameStatus::Malformed;
    for(;;){
        if(out.size()==MaximumNalCount)return FrameStatus::LimitExceeded;
        const auto body=at+prefix;std::size_t next=0,nextPrefix=0;const bool another=startCode(b,body,next,nextPrefix);
        const auto end=another?next:b.size;if(end-body<2)return FrameStatus::Malformed;
        const auto h0=b.data[body],h1=b.data[body+1];if(h0&0x80||!(h1&7))return FrameStatus::Malformed;
        out.push_back({at,body,end,unsigned((h0>>1)&63),unsigned(((h0&1)<<5)|(h1>>3)),unsigned(h1&7)});
        if(!another)break;
        at=next;prefix=nextPrefix;
    }return FrameStatus::Ok;
}
bool unescape(Bytes b,std::vector<std::uint8_t>& out){
    unsigned zeros=0;
    for(std::size_t i=0;i<b.size;++i){const auto c=b.data[i];
        if(zeros==2){if(c==3){if(i+1==b.size||b.data[i+1]>3)return false;zeros=0;continue;}if(c<3)return false;}
        out.push_back(c);zeros=c==0?std::min(2u,zeros+1):0;
    }return true;
}
FrameStatus locate(Bytes b,const std::vector<Nal>& ns,std::size_t& index,std::vector<std::uint8_t>& payload){
    bool found=false;
    for(std::size_t at=0;at<ns.size();++at){const auto& n=ns[at];if(n.type!=39&&n.type!=40)continue;
        auto end=n.end;while(end>n.body+2&&!b.data[end-1])--end;
        std::vector<std::uint8_t> rbsp;rbsp.reserve(end-n.body-2);
        if(!unescape({b.data+n.body+2,end-n.body-2},rbsp)||rbsp.empty()||rbsp.back()!=0x80)return FrameStatus::Malformed;
        std::size_t pos=0,messages=0;bool owned=false;
        while(pos<rbsp.size()-1){std::size_t type=0,size=0;
            for(;;){if(pos==rbsp.size()-1)return FrameStatus::Malformed;auto v=rbsp[pos++];type+=v;if(v!=255)break;}
            for(;;){if(pos==rbsp.size()-1)return FrameStatus::Malformed;auto v=rbsp[pos++];size+=v;if(v!=255)break;}
            if(size>rbsp.size()-1-pos)return FrameStatus::Malformed;
            ++messages;
            if(type==5&&size>=FrameUuid.size()&&std::equal(FrameUuid.begin(),FrameUuid.end(),rbsp.data()+pos)){
                if(found||owned)return FrameStatus::DuplicateOwned;
                if(n.type!=39)return FrameStatus::Malformed;
                owned=true;payload.assign(rbsp.begin()+std::ptrdiff_t(pos+FrameUuid.size()),rbsp.begin()+std::ptrdiff_t(pos+size));
            }pos+=size;
        }
        if(owned){if(messages!=1)return FrameStatus::Malformed;found=true;index=at;}
    }return found?FrameStatus::Ok:FrameStatus::NotFound;
}
void number(std::vector<std::uint8_t>& out,std::size_t n){while(n>=255){out.push_back(255);n-=255;}out.push_back(std::uint8_t(n));}
std::vector<std::uint8_t> encodeNal(Bytes payload,unsigned layer,unsigned temporal){
    std::vector<std::uint8_t> rbsp;number(rbsp,5);number(rbsp,FrameUuid.size()+payload.size);
    rbsp.insert(rbsp.end(),FrameUuid.begin(),FrameUuid.end());rbsp.insert(rbsp.end(),payload.data,payload.data+payload.size);rbsp.push_back(0x80);
    std::vector<std::uint8_t> out{0,0,0,1,std::uint8_t((39<<1)|(layer>>5)),std::uint8_t((layer<<3)|temporal)};unsigned zeros=0;
    for(auto c:rbsp){if(zeros==2&&c<=3){out.push_back(3);zeros=0;}out.push_back(c);zeros=c==0?std::min(2u,zeros+1):0;}return out;
}
bool descriptorValid(const FrameDescriptor& d){return d.sourceFrame&&d.encoderGeneration&&d.codedWidth&&d.codedHeight&&
    d.codedWidth<=8192&&d.codedHeight<=8192&&std::size_t(d.codedWidth)*d.codedHeight<=MaximumCoverageBytes;}
bool coverageValid(const FrameDescriptor& d,CoverageEncoding encoding,Bytes b){
    if(encoding==CoverageEncoding::None)return !b.size;
    if(!b.data||!b.size||b.size>MaximumCoverageBytes)return false;
    const auto size=std::size_t(d.codedWidth)*d.codedHeight;
    if(encoding==CoverageEncoding::Raw)return b.size==size;
    if(encoding!=CoverageEncoding::Rle||b.size%2)return false;
    std::size_t decoded=0;for(std::size_t i=0;i<b.size;i+=2){if(!b.data[i]||b.data[i]>size-decoded)return false;decoded+=b.data[i];}return decoded==size;
}
}
FrameStatus appendCertifiedFrame(Bytes original,const FrameDescriptor& d,CoverageEncoding encoding,
    Bytes coverage,AuthSession& auth,std::vector<std::uint8_t>& output){
    if(!descriptorValid(d)||!coverageValid(d,encoding,coverage))return FrameStatus::InvalidArgument;
    std::vector<Nal> ns;auto status=nals(original,ns);if(status!=FrameStatus::Ok)return status;
    std::size_t owned=0;std::vector<std::uint8_t> previous;status=locate(original,ns,owned,previous);
    if(status==FrameStatus::Ok)return FrameStatus::DuplicateOwned;
    if(status!=FrameStatus::NotFound)return status;
    const auto first=std::find_if(ns.begin(),ns.end(),[](const Nal& n){return n.type<=31;});if(first==ns.end())return FrameStatus::Malformed;
    std::vector<std::uint8_t> payload(FrameCertificateBytes+coverage.size,0);auto* h=payload.data();
    std::memcpy(h,"SFV1",4);h[4]=1;h[5]=std::uint8_t(encoding);put(h+6,FrameCertificateBytes,2);
    const auto pair=auth.pairId(),binding=auth.binding();std::copy(pair.begin(),pair.end(),h+8);std::copy(binding.begin(),binding.end(),h+24);
    put(h+40,d.sourceFrame,8);put(h+48,d.encoderGeneration,8);put(h+56,original.size,4);put(h+60,coverage.size,4);
    put(h+64,d.codedWidth,2);put(h+66,d.codedHeight,2);put(h+68,encoding==CoverageEncoding::None?0:std::size_t(d.codedWidth)*d.codedHeight,4);
    const auto videoHash=hash(original),coverageHash=hash(coverage);std::copy(videoHash.begin(),videoHash.end(),h+80);std::copy(coverageHash.begin(),coverageHash.end(),h+112);
    AuthTag tag{};if(auth.signFrameHeader({h,FrameSignedHeaderBytes},tag)!=AuthStatus::Ok)return FrameStatus::AuthenticationRefused;
    std::copy(tag.begin(),tag.end(),h+FrameSignedHeaderBytes);if(coverage.size)std::copy_n(coverage.data,coverage.size,h+FrameCertificateBytes);
    const auto added=encodeNal({payload.data(),payload.size()},first->layer,first->temporal);
    if(added.size()>MaximumAccessUnitBytes-original.size)return FrameStatus::LimitExceeded;
    std::vector<std::uint8_t> result;result.reserve(original.size+added.size());result.insert(result.end(),original.data,original.data+first->start);
    result.insert(result.end(),added.begin(),added.end());result.insert(result.end(),original.data+first->start,original.data+original.size);
    output.swap(result);return FrameStatus::Ok;
}
FrameStatus inspectCertifiedFrame(Bytes observed,AuthSession& auth,CertifiedAccessUnit& output){
    std::vector<Nal> ns;auto status=nals(observed,ns);if(status!=FrameStatus::Ok)return status;
    std::size_t owned=0;std::vector<std::uint8_t> payload;status=locate(observed,ns,owned,payload);if(status!=FrameStatus::Ok)return status;
    if(payload.size()<FrameCertificateBytes)return FrameStatus::Malformed;
    const auto* h=payload.data();
    if(std::memcmp(h,"SFV1",4)||h[4]!=1||h[5]>2||get(h+6,2)!=FrameCertificateBytes)return FrameStatus::Malformed;
    for(unsigned i=72;i<80;++i)if(h[i])return FrameStatus::Malformed;
    CertifiedAccessUnit result;result.descriptor={get(h+40,8),get(h+48,8),std::uint16_t(get(h+64,2)),std::uint16_t(get(h+66,2))};
    const auto& d=result.descriptor;result.coverageEncoding=CoverageEncoding(h[5]);
    if(!descriptorValid(d)||get(h+60,4)!=payload.size()-FrameCertificateBytes)return FrameStatus::Malformed;
    const auto rawSize=result.coverageEncoding==CoverageEncoding::None?0:std::size_t(d.codedWidth)*d.codedHeight;
    if(get(h+68,4)!=rawSize)return FrameStatus::Malformed;
    const Bytes coverage{h+FrameCertificateBytes,payload.size()-FrameCertificateBytes};
    if(!coverageValid(d,result.coverageEncoding,coverage))return FrameStatus::Malformed;
    if(auth.verifyFrameHeader({h,FrameSignedHeaderBytes},{h+FrameSignedHeaderBytes,AuthTagBytes},result.binding)!=AuthStatus::Ok)return FrameStatus::AuthenticationRefused;
    const auto& n=ns[owned];
    const auto first=std::find_if(ns.begin(),ns.end(),[](const Nal& item){return item.type<=31;});
    if(first==ns.end()||n.layer!=first->layer||n.temporal!=first->temporal)return FrameStatus::Malformed;
    result.original.reserve(observed.size-(n.end-n.start));
    result.original.insert(result.original.end(),observed.data,observed.data+n.start);result.original.insert(result.original.end(),observed.data+n.end,observed.data+observed.size);
    const auto videoHash=hash({result.original.data(),result.original.size()});if(get(h+56,4)!=result.original.size()||!equal({videoHash.data(),videoHash.size()},{h+80,32}))return FrameStatus::OriginalMismatch;
    const auto coverageHash=hash(coverage);if(!equal({coverageHash.data(),coverageHash.size()},{h+112,32}))return FrameStatus::CoverageMismatch;
    // A certificate cannot bless an empty/missing video payload.
    if(std::none_of(ns.begin(),ns.end(),[](const Nal& item){return item.type<=31;}))return FrameStatus::Malformed;
    if(coverage.size)result.coverage.assign(coverage.data,coverage.data+coverage.size);
    output=std::move(result);return FrameStatus::Ok;
}
bool encodeCoverageRle(Bytes raw,std::vector<std::uint8_t>& output){
    if(!raw.data||!raw.size||raw.size>MaximumCoverageBytes)return false;
    std::vector<std::uint8_t> result;result.reserve(std::min(raw.size,MaximumCoverageBytes));
    for(std::size_t at=0;at<raw.size;){std::size_t count=1;while(count<255&&at+count<raw.size&&raw.data[at+count]==raw.data[at])++count;
        if(result.size()>MaximumCoverageBytes-2)return false;
        result.push_back(std::uint8_t(count));result.push_back(raw.data[at]);at+=count;}
    output.swap(result);return true;
}
bool decodeCertifiedCoverage(const CertifiedAccessUnit& frame,std::vector<std::uint8_t>& output){
    const auto& d=frame.descriptor;Bytes b{frame.coverage.data(),frame.coverage.size()};
    if(!descriptorValid(d)||frame.coverageEncoding==CoverageEncoding::None||!coverageValid(d,frame.coverageEncoding,b))return false;
    std::vector<std::uint8_t> result;result.reserve(std::size_t(d.codedWidth)*d.codedHeight);
    if(frame.coverageEncoding==CoverageEncoding::Raw)result=frame.coverage;
    else for(std::size_t i=0;i<b.size;i+=2)result.insert(result.end(),b.data[i],b.data[i+1]);
    output.swap(result);return true;
}
} // namespace stream_bridge
