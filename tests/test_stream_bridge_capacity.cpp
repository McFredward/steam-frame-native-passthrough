#include "bridge/stream_bridge_capacity.h"
#include <cstdio>
#include <cstdlib>
namespace sb=stream_bridge;
unsigned checks=0;void check(bool b,const char* text){++checks;if(!b){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
int main(){
    sb::Pairing p{},r{};p.role=sb::Role::Producer;r.role=sb::Role::Receiver;
    for(unsigned i=0;i<16;++i)p.pairId[i]=r.pairId[i]=std::uint8_t(i+1);
    for(unsigned i=0;i<32;++i)p.key[i]=r.key[i]=std::uint8_t(i+40);
    sb::CapacityProtection producer,receiver;check(producer.prepare(p)&&receiver.prepare(r),"real role-paired protectors prepared");
    sb::CapacityMessage challenge;challenge.binding.fill(12);challenge.challenge.fill(14);sb::AuthEnvelope wire{};
    check(producer.seal(challenge,wire),"fresh challenge protected");sb::CapacityMessage observed;
    check(receiver.open({wire.data(),wire.size()},observed)&&observed.binding==challenge.binding&&observed.challenge==challenge.challenge,"challenge exact roundtrip without video bootstrap");
    for(std::size_t i=0;i<wire.size();++i){auto changed=wire;changed[i]^=64;sb::CapacityMessage held;held.maximumAccessUnitBytes=73;
        check(!receiver.open({changed.data(),changed.size()},held)&&held.maximumAccessUnitBytes==73,"tampered capacity cannot publish source admission");}
    observed.role=sb::Role::Receiver;observed.source.connection.fill(19);observed.source.generation=3;observed.maximumAccessUnitBytes=1024*1024;
    check(receiver.seal(observed,wire)&&producer.open({wire.data(),wire.size()},challenge)&&challenge.maximumAccessUnitBytes==1024*1024,"actual native source capacity replied in independent protected domain");
    check(!receiver.open({wire.data(),wire.size()},challenge),"reflection rejected");
    for(const auto bytes:{0u,511u,16u*1024*1024+1}){observed.maximumAccessUnitBytes=bytes;check(!receiver.seal(observed,wire),"missing/unbounded capacity cannot be signed");}
    producer.reset();check(!producer.open({wire.data(),wire.size()},challenge),"reset revokes capacity protection");
    std::printf("%u capacity control checks passed\n",checks);
}
