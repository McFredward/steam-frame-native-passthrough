#include "stream_bridge_capacity.h"
#include "hevc_bridge_sei.h"
#include <algorithm>
#include <cstring>
namespace stream_bridge {
namespace {
bool nonzero(const Nonce& n){return std::any_of(n.begin(),n.end(),[](auto b){return b!=0;});}
void put(std::uint8_t* p,std::uint64_t n,unsigned count){for(unsigned i=0;i<count;++i)p[i]=std::uint8_t(n>>(8*i));}
std::uint64_t get(const std::uint8_t* p,unsigned count){std::uint64_t n=0;for(unsigned i=0;i<count;++i)n|=std::uint64_t(p[i])<<(8*i);return n;}
bool valid(const CapacityMessage& m){
    if(!nonzero(m.binding)||!nonzero(m.challenge))return false;
    if(m.role==Role::Producer)return !nonzero(m.source.connection)&&!m.source.generation&&!m.maximumAccessUnitBytes;
    return m.role==Role::Receiver&&nonzero(m.source.connection)&&m.source.generation&&
        m.maximumAccessUnitBytes>=512&&m.maximumAccessUnitBytes<=MaximumAccessUnitBytes;
}
bool tag(const Secret& key,Bytes header,AuthTag& out){
    constexpr char domain[]="SFNP-DECODER-BUDGET-V1";
    std::array<std::uint8_t,sizeof(domain)-1+144> data{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(domain),sizeof(domain)-1,data.begin());
    std::copy_n(header.data,144,data.begin()+sizeof(domain)-1);
    return hmacSha256({key.data(),key.size()},{data.data(),data.size()},out)==AuthStatus::Ok;
}
}
CapacityProtection::~CapacityProtection(){reset();}
void CapacityProtection::reset() noexcept{secureZero(pair_.key.data(),pair_.key.size());pair_={};prepared_=false;}
bool CapacityProtection::prepare(const Pairing& pair) noexcept{
    reset();if((pair.role!=Role::Producer&&pair.role!=Role::Receiver)||!nonzero(pair.pairId)||
        !std::any_of(pair.key.begin(),pair.key.end(),[](auto b){return b!=0;}))return false;
    pair_=pair;prepared_=true;return true;
}
bool CapacityProtection::seal(const CapacityMessage& m,AuthEnvelope& out) const noexcept{
    if(!prepared_||m.role!=pair_.role||!valid(m))return false;
    AuthEnvelope data{};std::memcpy(data.data(),"SFC1",4);data[4]=1;data[5]=std::uint8_t(m.role);put(data.data()+6,data.size(),2);
    std::copy(pair_.pairId.begin(),pair_.pairId.end(),data.begin()+8);
    std::copy(m.binding.begin(),m.binding.end(),data.begin()+24);std::copy(m.challenge.begin(),m.challenge.end(),data.begin()+40);
    std::copy(m.source.connection.begin(),m.source.connection.end(),data.begin()+56);put(data.data()+72,m.source.generation,8);put(data.data()+80,m.maximumAccessUnitBytes,4);
    AuthTag mac{};if(!tag(pair_.key,{data.data(),144},mac))return false;
    std::copy(mac.begin(),mac.end(),data.begin()+144);out=data;return true;
}
bool CapacityProtection::open(Bytes data,CapacityMessage& out) const noexcept{
    if(!prepared_||!data.data||data.size!=AuthEnvelopeBytes||std::memcmp(data.data,"SFC1",4)||data.data[4]!=1||get(data.data+6,2)!=data.size)return false;
    if(!std::equal(pair_.pairId.begin(),pair_.pairId.end(),data.data+8))return false;
    for(unsigned i=84;i<144;++i)if(data.data[i])return false;
    CapacityMessage m;m.role=Role(data.data[5]);
    std::copy_n(data.data+24,16,m.binding.begin());std::copy_n(data.data+40,16,m.challenge.begin());std::copy_n(data.data+56,16,m.source.connection.begin());
    m.source.generation=get(data.data+72,8);m.maximumAccessUnitBytes=std::uint32_t(get(data.data+80,4));
    if(m.role==pair_.role||!valid(m))return false;
    AuthTag mac{};if(!tag(pair_.key,{data.data,144},mac))return false;std::uint8_t delta=0;
    for(unsigned i=0;i<32;++i)delta|=mac[i]^data.data[144+i];
    if(delta)return false;
    out=m;return true;
}
}
