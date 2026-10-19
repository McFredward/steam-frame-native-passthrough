#include "pc/stream-bridge-source-contract.h"
#include "pc/stream-bridge-sender-provider-sourceipc.h"
#include <cstdio>
#include <stdexcept>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace {
unsigned checks=0;void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
void run(){
 SourceContract contract;SourceIpcPermission pipe;SourceIpcRequest app;app.processId=73;app.nonce[0]=9;
 SourceIpcState state;state.generation=4;state.binding[0]=7;state.compatible=true;
 const auto unavailable=contract.inspect(0,73);
 check(unavailable.status==SourceContractStatus::NoDeclaration&&!unavailable.current()&&!unavailable.declaredProcess&&unavailable.nativeProcess==73,"absent format declaration is recorded by the same atomic observation that refuses it");
 check(!pipe.process()&&!contract.snapshot(73),"no application request or prior grant cannot authorize source format");
 pipe.update(app,51,{},true);contract.publish(pipe.process());
 check(!contract.snapshot(73),"native texture preparation alone without peer confirmation cannot authorize format");
 pipe.update(app,51,state,false);contract.publish(pipe.process());
 check(!contract.snapshot(73),"peer confirmation alone without actual original source preparation cannot authorize format");
 pipe.update(app,51,state,true);contract.publish(pipe.process());const auto first=contract.snapshot(73);
 check(first&&contract.current(first,73),"kernel-validated current ordinary-RGBA source admits its exact captured receipt");
 const auto current=contract.inspect(first,73);
 check(current.current()&&current.declaredProcess==73&&current.nativeProcess==73,"current declaration observation preserves both matching process identities");
 const auto foreign=contract.inspect(first,74);
 check(foreign.status==SourceContractStatus::DifferentProcess&&!foreign.current()&&foreign.declaredProcess==73&&foreign.nativeProcess==74,"foreign native loading source is distinguishable from an absent application declaration without borrowing its permission");
 check(contract.inspect(0,73).status==SourceContractStatus::NoDeclaration,"a capture made before admission is not mislabeled as a revoked old generation");
 check(!contract.snapshot(74)&&!contract.current(first,74)&&!contract.current(0,73),"foreign or absent source process can never borrow another app permission");
 for(unsigned i=0;i<1000;++i){contract.publish(pipe.process());check(contract.snapshot(73)==first,"unchanged live permission never changes the captured epoch");}
 pipe.update(app,51,{},false);contract.publish(pipe.process());
 check(contract.current(first,73),"source-format lease survives native encoder renegotiation, not image/peer authority");
 pipe.reset();contract.publish(pipe.process());check(!contract.current(first,73)&&!contract.snapshot(73),"actual pipe cancellation immediately revokes all frozen old source receipts");
 app.nonce[0]=10;pipe.update(app,51,state,true);contract.publish(pipe.process());const auto second=contract.snapshot(73);
 check(second&&second!=first&&!contract.current(first,73),"same kernel PID reconnect requires a fresh source declaration epoch");
 const auto revoked=contract.inspect(first,73);
 check(revoked.status==SourceContractStatus::RevokedGeneration&&!revoked.current()&&revoked.declaredProcess==73&&revoked.nativeProcess==73,"same process with an old captured generation is distinct from a foreign native source");
 contract.publish(91);check(!contract.current(second,73)&&!contract.snapshot(73)&&contract.snapshot(91),"switching applications cannot retain old captured source authority");
 contract.publish(73);check(contract.snapshot(73)!=second,"PID reuse never revives its former source receipt");
 SourceContract exhausted(UINT32_MAX-1);check(exhausted.publish(1)&&exhausted.snapshot(1),"last nonwrapping declaration epoch is usable");
 check(!exhausted.publish(0)&&!exhausted.snapshot(1),"revocation at exhaustion remains immediate and opaque");
 check(!exhausted.publish(1)&&!exhausted.snapshot(1),"exhaustion cannot wrap and resurrect an old receipt");
}
}
int main(){try{run();std::printf("actual source contract/IPC permission: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"source contract failed after %u: %s\n",checks,e.what());return 1;}}
