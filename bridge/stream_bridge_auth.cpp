#include "stream_bridge_auth.h"
#include "stream_bridge_frame.h"
#include "../receiver/sha256.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace stream_bridge {
namespace {
template<std::size_t N> bool nonzero(const std::array<std::uint8_t,N>& b) noexcept {
    std::uint8_t v=0; for(auto x:b)v|=x; return v!=0;
}
void put(std::uint8_t* p,std::uint64_t value,unsigned count) noexcept {
    for(unsigned i=0;i<count;++i)p[i]=static_cast<std::uint8_t>(value>>(8*i));
}
std::uint64_t get(const std::uint8_t* p,unsigned count) noexcept {
    std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t(p[i])<<(8*i);return value;
}
bool roleValid(Role role) noexcept { return role==Role::Producer||role==Role::Receiver; }
bool sameSource(const VideoStreamContext& a,const VideoStreamContext& b) noexcept {
    return a.connection==b.connection&&a.generation==b.generation;
}
bool sourceValid(const VideoStreamContext& s) noexcept { return nonzero(s.connection)&&s.generation; }
bool pairingValid(const Pairing& p,Role expected) noexcept {
    return p.role==expected&&nonzero(p.pairId)&&nonzero(p.key);
}
bool payloadValid(Bytes payload,Role expected,bool initialHello) noexcept {
    if(!payload.data||payload.size!=MessageBytes)return false;
    Message m;
    if(decode(payload,m)==Status::Ok)
        return m.role==expected&&(!initialHello||m.kind==Kind::Hello);
    if(initialHello)return false;
    ProbeMessage probe;
    if(decodeProbe(payload,probe)!=Status::Ok)return false;
    return (probe.kind==ProbeKind::Probe&&expected==Role::Producer)
        ||(probe.kind==ProbeKind::Echo&&expected==Role::Receiver);
}
bool constantTimeEqual(const AuthTag& expected,const std::uint8_t* actual) noexcept {
    std::uint8_t difference=0;
    for(std::size_t i=0;i<expected.size();++i)difference|=expected[i]^actual[i];
    return difference==0;
}
// Canonical KDF input: domain (without NUL), pair ID, binding, direction byte.
AuthStatus derive(const Secret& secret,const Nonce& pair,const Nonce& binding,
                  Role direction,Secret& key) noexcept {
    constexpr char domain[]="SFNP-STREAM-BRIDGE-AUTH-V1";
    std::array<std::uint8_t,sizeof(domain)-1+16+16+1> data{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(domain),sizeof(domain)-1,data.begin());
    auto at=data.begin()+sizeof(domain)-1;
    at=std::copy(pair.begin(),pair.end(),at);at=std::copy(binding.begin(),binding.end(),at);
    *at=static_cast<std::uint8_t>(direction);
    const auto result=hmacSha256({secret.data(),secret.size()},{data.data(),data.size()},key);
    secureZero(data.data(),data.size());return result;
}
AuthStatus tagFor(const Secret& secret,const Nonce& pair,const Nonce& binding,
                  Role direction,const std::uint8_t* headerPayload,AuthTag& tag) noexcept {
    Secret directionKey{};
    auto result=derive(secret,pair,binding,direction,directionKey);
    if(result==AuthStatus::Ok)result=hmacSha256({directionKey.data(),directionKey.size()},
        {headerPayload,AuthHeaderBytes+MessageBytes},tag);
    secureZero(directionKey.data(),directionKey.size());return result;
}
}
void secureZero(void* data,std::size_t size) noexcept {
    auto* p=static_cast<volatile std::uint8_t*>(data);
    if(p)while(size--)*p++=0;
}
AuthStatus hmacSha256(Bytes key,Bytes data,AuthTag& output) noexcept {
    if((!key.data&&key.size)||(!data.data&&data.size)||key.size>1024||data.size>4096)
        return AuthStatus::InvalidArgument;
    std::array<std::uint8_t,64> block{},ipad{},opad{};
    AuthTag hashedKey{},innerDigest{},result{};
    if(key.size>block.size()) {
        frame_receiver::Sha256 hash;hash.update(key.data,key.size);hashedKey=hash.finish();
        secureZero(&hash,sizeof(hash));std::copy(hashedKey.begin(),hashedKey.end(),block.begin());
    } else if(key.size)std::copy_n(key.data,key.size,block.begin());
    for(std::size_t i=0;i<block.size();++i){ipad[i]=block[i]^0x36;opad[i]=block[i]^0x5c;}
    frame_receiver::Sha256 inner;inner.update(ipad.data(),ipad.size());
    if(data.size)inner.update(data.data,data.size);
    innerDigest=inner.finish();secureZero(&inner,sizeof(inner));
    frame_receiver::Sha256 outer;outer.update(opad.data(),opad.size());
    outer.update(innerDigest.data(),innerDigest.size());result=outer.finish();secureZero(&outer,sizeof(outer));
    output=result;
    secureZero(block.data(),block.size());secureZero(ipad.data(),ipad.size());secureZero(opad.data(),opad.size());
    secureZero(hashedKey.data(),hashedKey.size());secureZero(innerDigest.data(),innerDigest.size());
    secureZero(result.data(),result.size());return AuthStatus::Ok;
}
AuthSession::~AuthSession() { reset();secureZero(this,sizeof(*this)); }
void AuthSession::reset() noexcept {
    if(bound_){previousPair_=pairId_;previousBinding_=binding_;}
    secureZero(key_.data(),key_.size());pairId_={};binding_={};source_={};
    role_=Role::Producer;sent_=received_=0;prepared_=bound_=peerVerified_=false;
}
AuthStatus AuthSession::prepareProducer(const Pairing& pairing,const Nonce& binding) noexcept {
    if(!pairingValid(pairing,Role::Producer)||!nonzero(binding)
        ||(pairing.pairId==previousPair_&&binding==previousBinding_)
        ||(bound_&&pairing.pairId==pairId_&&binding==binding_))return AuthStatus::InvalidArgument;
    reset();key_=pairing.key;pairId_=pairing.pairId;binding_=binding;
    role_=Role::Producer;prepared_=bound_=true;return AuthStatus::Ok;
}
AuthStatus AuthSession::prepareReceiver(const Pairing& pairing,const VideoStreamContext& source) noexcept {
    if(!pairingValid(pairing,Role::Receiver)||!sourceValid(source))return AuthStatus::InvalidArgument;
    reset();key_=pairing.key;pairId_=pairing.pairId;source_=source;
    role_=Role::Receiver;prepared_=true;return AuthStatus::Ok;
}
AuthStatus AuthSession::localSendAuthority(LocalSendAuthority& output) const noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(!bound_)return AuthStatus::NotBound;
    output.channel={true,binding_};return AuthStatus::Ok;
}
AuthStatus AuthSession::seal(Bytes payload,AuthEnvelope& output) noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(!bound_)return AuthStatus::NotBound;
    if(!payloadValid(payload,role_,false))return AuthStatus::PayloadInvalid;
    if(sent_==std::numeric_limits<std::uint64_t>::max())return AuthStatus::SequenceExhausted;
    AuthEnvelope result{};
    result[0]='S';result[1]='F';result[2]='A';result[3]='1';
    put(result.data()+4,1,2);put(result.data()+6,0,2);result[8]=static_cast<std::uint8_t>(role_);
    put(result.data()+10,AuthHeaderBytes,2);put(result.data()+12,MessageBytes,2);
    put(result.data()+14,AuthEnvelopeBytes,2);
    std::copy(pairId_.begin(),pairId_.end(),result.begin()+16);
    std::copy(binding_.begin(),binding_.end(),result.begin()+32);
    put(result.data()+48,sent_+1,8);std::copy_n(payload.data,MessageBytes,result.begin()+AuthHeaderBytes);
    AuthTag tag{};const auto status=tagFor(key_,pairId_,binding_,role_,result.data(),tag);
    if(status!=AuthStatus::Ok)return status;
    std::copy(tag.begin(),tag.end(),result.begin()+AuthHeaderBytes+MessageBytes);
    secureZero(tag.data(),tag.size());output=result;++sent_;return AuthStatus::Ok;
}
AuthStatus AuthSession::inspect(Bytes envelope,const Nonce* expected,AuthPayload& payload,
    Nonce& binding,std::uint64_t& sequence,bool initialHello) const noexcept {
    if(!envelope.data)return AuthStatus::InvalidArgument;
    if(envelope.size!=AuthEnvelopeBytes)return AuthStatus::Malformed;
    const auto* p=envelope.data;
    if(p[0]!='S'||p[1]!='F'||p[2]!='A'||p[3]!='1'||p[9]
        ||get(p+10,2)!=AuthHeaderBytes||get(p+12,2)!=MessageBytes
        ||get(p+14,2)!=AuthEnvelopeBytes)return AuthStatus::Malformed;
    for(unsigned i=56;i<64;++i)if(p[i])return AuthStatus::Malformed;
    if(get(p+4,2)!=1||get(p+6,2)!=0)return AuthStatus::UnsupportedVersion;
    const auto direction=static_cast<Role>(p[8]);
    if(!roleValid(direction)||direction==role_)return AuthStatus::RoleMismatch;
    Nonce pair{},candidate{};std::copy_n(p+16,16,pair.begin());std::copy_n(p+32,16,candidate.begin());
    if(pair!=pairId_)return AuthStatus::PairMismatch;
    if(!nonzero(candidate)||(expected&&candidate!=*expected)
        ||(!expected&&pair==previousPair_&&candidate==previousBinding_))return AuthStatus::BindingMismatch;
    const auto number=get(p+48,8);
    if(!number||number<=received_)return AuthStatus::Replay;
    AuthTag tag{};const auto status=tagFor(key_,pair,candidate,direction,p,tag);
    if(status!=AuthStatus::Ok)return status;
    const bool matches=constantTimeEqual(tag,p+AuthHeaderBytes+MessageBytes);
    secureZero(tag.data(),tag.size());if(!matches)return AuthStatus::BadMac;
    const Bytes body{p+AuthHeaderBytes,MessageBytes};
    if(!payloadValid(body,direction,initialHello))return AuthStatus::PayloadInvalid;
    AuthPayload result{};std::copy_n(body.data,MessageBytes,result.begin());
    payload=result;binding=candidate;sequence=number;return AuthStatus::Ok;
}
AuthStatus AuthSession::bootstrapFromVideo(Bytes envelope,const VideoSourceEvidence& evidence,
    AuthPayload& payload,VerifiedChannel& channel) noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(role_!=Role::Receiver)return AuthStatus::RoleMismatch;
    if(bound_)return AuthStatus::AlreadyBound;
    if(!evidence.currentCompleteAccessUnit||!sourceValid(evidence.stream)||!sameSource(source_,evidence.stream))
        return AuthStatus::SourceUnproved;
    AuthPayload result{};Nonce binding{};std::uint64_t sequence=0;
    const auto status=inspect(envelope,nullptr,result,binding,sequence,true);
    if(status!=AuthStatus::Ok)return status;
    binding_=binding;received_=sequence;bound_=peerVerified_=true;
    payload=result;channel={true,binding_};return AuthStatus::Ok;
}
AuthStatus AuthSession::openControl(Bytes envelope,AuthPayload& payload,VerifiedChannel& channel) noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(!bound_)return AuthStatus::NotBound;
    AuthPayload result{};Nonce binding{};std::uint64_t sequence=0;
    const auto status=inspect(envelope,&binding_,result,binding,sequence,false);
    if(status!=AuthStatus::Ok)return status;
    received_=sequence;peerVerified_=true;payload=result;channel={true,binding_};return AuthStatus::Ok;
}
namespace {
AuthStatus frameHeaderTag(const Secret& key,Bytes header,AuthTag& tag) noexcept {
    constexpr char domain[]="SFNP-EXACT-ACCESS-UNIT-V1";
    std::array<std::uint8_t,sizeof(domain)-1+FrameSignedHeaderBytes> bytes{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(domain),sizeof(domain)-1,bytes.begin());
    std::copy_n(header.data,header.size,bytes.begin()+sizeof(domain)-1);
    return hmacSha256({key.data(),key.size()},{bytes.data(),bytes.size()},tag);
}
bool frameHeaderShape(Bytes header) noexcept {
    return header.data&&header.size==FrameSignedHeaderBytes&&
        !std::memcmp(header.data,"SFV1",4)&&header.data[4]==1&&header.data[5]<=std::uint8_t(CoverageEncoding::RleZstd)&&
        get(header.data+6,2)==FrameCertificateBytes;
}
}
AuthStatus AuthSession::signFrameHeader(Bytes header,AuthTag& tag) const noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(role_!=Role::Producer)return AuthStatus::RoleMismatch;
    if(!bound_)return AuthStatus::NotBound;
    if(!frameHeaderShape(header))return AuthStatus::Malformed;
    if(!std::equal(pairId_.begin(),pairId_.end(),header.data+8))return AuthStatus::PairMismatch;
    if(!std::equal(binding_.begin(),binding_.end(),header.data+24))return AuthStatus::BindingMismatch;
    return frameHeaderTag(key_,header,tag);
}
AuthStatus AuthSession::verifyFrameHeader(Bytes header,Bytes tag,Nonce& binding) const noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(role_!=Role::Receiver)return AuthStatus::RoleMismatch;
    if(!frameHeaderShape(header)||!tag.data||tag.size!=AuthTagBytes)return AuthStatus::Malformed;
    if(!std::equal(pairId_.begin(),pairId_.end(),header.data+8))return AuthStatus::PairMismatch;
    Nonce candidate{};std::copy_n(header.data+24,candidate.size(),candidate.begin());
    if(!nonzero(candidate)||(bound_&&candidate!=binding_)||
        (!bound_&&pairId_==previousPair_&&candidate==previousBinding_))return AuthStatus::BindingMismatch;
    AuthTag expected{};auto result=frameHeaderTag(key_,header,expected);
    if(result==AuthStatus::Ok&&!constantTimeEqual(expected,tag.data))result=AuthStatus::BadMac;
    secureZero(expected.data(),expected.size());
    if(result==AuthStatus::Ok)binding=candidate;
    return result;
}
AuthStatus AuthSession::verifyInitialVideoHello(Bytes envelope,const Nonce& certifiedBinding) const noexcept {
    if(!prepared_)return AuthStatus::NotPrepared;
    if(role_!=Role::Receiver)return AuthStatus::RoleMismatch;
    if(bound_)return AuthStatus::AlreadyBound;
    AuthPayload body{};Nonce binding{};std::uint64_t sequence=0;
    const auto status=inspect(envelope,nullptr,body,binding,sequence,true);
    return status==AuthStatus::Ok&&binding!=certifiedBinding?AuthStatus::BindingMismatch:status;
}
} // namespace stream_bridge
