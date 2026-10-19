#include "pc/stream-bridge-sender-provider-sourceipc.h"
#include <cstdio>
#include <stdexcept>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace {unsigned checks=0;void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void run(){
 SourceIpcMessage request{};const std::uint8_t golden[24]{0x53,0x42,0x4e,0x31,1,0,1,0,128,0,0,0,1,0,0,0,0x78,0x56,0x34,0x12,0,0,0,0};std::memcpy(request.data(),golden,24);request[24]=17;request[39]=99;
 SourceIpcRequest parsed;check(readSourceRequest(request,0x12345678,parsed),"hand checked SBN1 golden accepted");check(parsed.processId==0x12345678&&parsed.nonce[0]==17&&parsed.nonce[15]==99,"exact kernel PID and nonce kept");
 for(unsigned byte:{0u,4u,6u,8u,10u,12u,16u,20u,40u,63u,127u}){auto bad=request;bad[byte]^=1;SourceIpcRequest original=parsed;check(!readSourceRequest(bad,0x12345678,parsed),"malformed version/flags/PID/reserved refused");check(parsed.processId==original.processId&&parsed.nonce==original.nonce,"invalid request cannot change authority");}
 check(!readSourceRequest(request,0x12345679,parsed)&&!readSourceRequest(request,0,parsed),"kernel client PID cannot be replaced by payload");auto empty=request;for(unsigned i=24;i<40;++i)empty[i]=0;check(!readSourceRequest(empty,0x12345678,parsed),"zero nonce refused");
 Nonce binding{};binding[0]=21;auto reply=sourceReply(parsed,0x87654321,binding,0x0102030405060708ull,true);check(reply[4]==2&&reply[10]==1&&reply[20]==0x21&&reply[23]==0x87&&reply[24]==17&&reply[39]==99&&reply[40]==21&&reply[56]==8&&reply[63]==1,"reply handchecked LE fields, echoed nonce and source preparation flag");

 SourceIpcPermission permission;SourceIpcState waiting{},compatible{binding,17,true};
 check(permission.update(parsed,77,waiting,true)[10]==0,"no historical grant: missing authenticated peer stays unavailable");
 check(permission.update(parsed,77,compatible,false)[10]==0,"no historical grant: prepared source required");
 auto granted=permission.update(parsed,77,compatible,true);check(granted[10]==1&&granted[56]==17,"proper first grant is source-format flag1 only");
 check(permission.update(parsed,77,waiting,false)==granted,"pool/encoder renegotiation cannot flip ordinary RGBA back to atlas");
 auto fresh=compatible;fresh.generation=18;fresh.binding[0]=22;
 check(permission.update(parsed,77,fresh,false)==granted&&permission.update(parsed,77,fresh,true)==granted,"fresh encoder proof cannot rewrite the live source-format lease");
 auto other=parsed;other.nonce[0]^=1;
 check(permission.update(other,77,waiting,true)[10]==0,"new nonce cannot inherit prior grant");
 permission.update(parsed,77,compatible,true);other=parsed;++other.processId;
 check(permission.update(other,77,waiting,true)[10]==0,"new kernel-validated PID cannot inherit prior grant");
 permission.update(parsed,77,compatible,true);
 check(permission.update(parsed,78,waiting,true)[10]==0,"new server identity cannot inherit prior grant");
 permission.update(parsed,77,compatible,true);permission.reset();
 check(permission.update(parsed,77,waiting,true)[10]==0,"cancel/reap/stop reset cannot leak a grant on reconnect");
 auto emptyClient=parsed;emptyClient.nonce={};
 check(permission.update(emptyClient,77,compatible,true)[10]==0,"empty client nonce cannot get source permission");
 check(permission.update(parsed,0,compatible,true)[10]==0,"absent server identity cannot get source permission");
 for(unsigned i=64;i<128;++i)check(reply[i]==0,"all response reserved bytes zero");
 check(reply[10]!=3,"local preparation never claims selected-image operational proof");
 for(const auto& denied:{sourceReply(parsed,1,{},3,true),sourceReply(parsed,1,binding,0,true),sourceReply(parsed,1,binding,3,false)}){check(denied[10]==0,"missing generation/binding/permission stays ordinary");for(unsigned i=40;i<64;++i)check(denied[i]==0,"no leaked stale source binding in refused reply");}
}}
int main(){try{run();std::printf("native app source IPC framing: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"source IPC after %u: %s\n",checks,e.what());return 1;}}
