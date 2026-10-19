#include "../receiver/stream_bridge_native_loader.h"
#include <array>
#include <cstdio>
#include <stdexcept>
namespace nd=stream_bridge_native;
namespace {unsigned checks=0;void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Actor{unsigned calls=0,fail=99;std::uintptr_t* foreign=nullptr;};
bool exchange(std::uintptr_t* p,std::uintptr_t& expected,std::uintptr_t next,void* context)noexcept{
    auto& a=*static_cast<Actor*>(context);if(++a.calls==a.fail){if(a.foreign)*a.foreign=999;return false;}
    if(*p!=expected){expected=*p;return false;}*p=next;return true;
}
}
int main(){try{
    std::array<std::uintptr_t,4> words{{11,22,33,44}};std::array<nd::ImportSlot,4> slots{};
    for(std::size_t i=0;i<4;++i)slots[i]={&words[i],words[i],words[i]+100};
    Actor actor;check(nd::publishSlots(slots,exchange,&actor),"all exact admitted slots publish");
    check(words==std::array<std::uintptr_t,4>{{111,122,133,144}},"published values exact");
    nd::restoreSlots(slots,exchange,&actor);check(words==std::array<std::uintptr_t,4>{{11,22,33,44}},"owned restore exact");
    for(unsigned fail=1;fail<=4;++fail){actor={0,fail,nullptr};check(!nd::publishSlots(slots,exchange,&actor),"individual slot conflict refuses");
        check(words==std::array<std::uintptr_t,4>{{11,22,33,44}},"partial publication rolls back only owned prefix");}
    actor={0,3,&words[0]};check(!nd::publishSlots(slots,exchange,&actor),"concurrent actor conflict refuses");
    check(words[0]==999&&words[1]==22&&words[2]==33&&words[3]==44,"rollback preserves actor replacement");
    words[0]=11;auto bad=slots;bad[3].address=bad[0].address;actor={};
    check(!nd::publishSlots(bad,exchange,&actor)&&actor.calls==0,"duplicate slot rejected before mutation");
    bad=slots;bad[2].replacement=0;check(!nd::publishSlots(bad,exchange,&actor)&&actor.calls==0,"missing entry rejected before mutation");
    check(!nd::publishPinnedDecodeImports()&&!nd::decodeImportsPublished(),"actual unrelated test executable cannot publish native GOT");
    std::printf("native import publication: %u checks PASS\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
