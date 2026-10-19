#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "pc/stream-bridge-sender-provider-sourceipc.h"
#include "pc/stream-bridge-source-contract.h"
#include <cstdio>
#include <stdexcept>
#include <cwchar>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace {
unsigned checks=0;void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
struct State {SourceContract contract;bool ready=false;unsigned grants=0,revokes=0,preparations=0;};
struct Client {
    HANDLE pipe=INVALID_HANDLE_VALUE,event=nullptr;OVERLAPPED write{};
    explicit Client(const wchar_t* name){pipe=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);check(pipe!=INVALID_HANDLE_VALUE,"actual local pipe opens");event=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(event!=nullptr,"actual client completion event");write.hEvent=event;}
    ~Client(){if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);if(event)CloseHandle(event);}
    void send(const SourceIpcMessage& request){DWORD bytes=0;check(WriteFile(pipe,request.data(),static_cast<DWORD>(SourceIpcBytes),&bytes,&write)||GetLastError()==ERROR_IO_PENDING,"actual request enters native pipe");}
    SourceIpcMessage receive(SourceIpcServer& server,const SourceIpcState& state){
        HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(done!=nullptr,"actual reply event");OVERLAPPED read{};read.hEvent=done;SourceIpcMessage reply{};DWORD bytes=0;
        bool complete=ReadFile(pipe,reply.data(),static_cast<DWORD>(SourceIpcBytes),&bytes,&read)!=0;
        check(complete||GetLastError()==ERROR_IO_PENDING,"actual reply read starts");
        const auto deadline=GetTickCount64()+3000;
        while(!complete&&GetTickCount64()<deadline){server.tick(state);complete=GetOverlappedResult(pipe,&read,&bytes,FALSE)!=0;if(!complete)check(GetLastError()==ERROR_IO_INCOMPLETE,"actual reply has no unexpected pipe error");Sleep(1);}
        if(!complete){CancelIoEx(pipe,&read);GetOverlappedResult(pipe,&read,&bytes,TRUE);}
        CloseHandle(done);check(complete&&bytes==SourceIpcBytes,"actual server returns a complete bounded reply");return reply;
    }
};
SourceIpcMessage request(std::uint32_t pid,unsigned nonce){SourceIpcMessage out{};std::memcpy(out.data(),"SBN1",4);out[4]=1;out[6]=1;out[8]=128;out[12]=1;for(unsigned i=0;i<4;++i)out[16+i]=std::uint8_t(pid>>(8*i));out[24]=std::uint8_t(nonce);return out;}
void run(){
 State observed;SourceIpcServer server([](std::uint32_t,void* p){auto&s=*static_cast<State*>(p);++s.preparations;return s.ready;},&observed,
     [](std::uint32_t pid,void* p)noexcept{auto&s=*static_cast<State*>(p);s.contract.publish(pid);if(pid)++s.grants;else ++s.revokes;});
 SourceIpcState peer;peer.compatible=true;peer.generation=19;peer.binding[0]=31;
 wchar_t name[96]{};std::swprintf(name,96,L"\\\\.\\pipe\\SFNP.StreamBridge.V1.%lu",GetCurrentProcessId());
 server.tick(peer);const auto pid=GetCurrentProcessId();std::uint64_t first=0;
 {
     Client client(name);const auto input=request(pid,7);client.send(input);
     check(client.receive(server,peer)[10]==0&&!observed.contract.snapshot(pid),"kernel client with no prepared source stays opaque");
     observed.ready=true;
     check(client.receive(server,peer)[10]==1,"same actual pipe receives ordinary-RGBA permission after preparation");
     first=observed.contract.snapshot(pid);check(first&&observed.grants==1,"real kernel PID activates exactly one source declaration");
     const auto preparations=observed.preparations;
     for(unsigned i=0;i<1000;++i)server.tick(peer);
     check(observed.preparations==preparations,"live pipe grant never repeatedly contends for native render metadata");
     server.tick({});check(observed.contract.current(first,pid),"encoder loss does not revoke the live app format contract");
 }
 const auto closedDeadline=GetTickCount64()+3000;
 while(observed.contract.snapshot(pid)&&GetTickCount64()<closedDeadline){server.tick(peer);Sleep(1);}
 check(!observed.contract.current(first,pid)&&observed.revokes==1,"real local EOF revokes frozen old source receipts");
 for(unsigned i=0;i<10;++i)server.tick(peer);
 {
     const auto preparations=observed.preparations;
     Client client(name);const auto input=request(pid,8);client.send(input);check(client.receive(server,peer)[10]==1,"same actual kernel process reconnects with a new nonce");
     check(observed.preparations>preparations,"new live client must revalidate native source preparation after EOF");
     check(observed.contract.snapshot(pid)!=first&&!observed.contract.current(first,pid),"native reconnect cannot revive the old capture declaration");
     server.stop();check(!observed.contract.snapshot(pid),"explicit native stop revokes immediately without another tick");
 }
}
}
int main(){try{run();std::printf("actual Windows pipe source declaration: %u checks PASS\n",checks);return 0;}catch(const std::exception&e){std::fprintf(stderr,"source pipe failed after %u: %s\n",checks,e.what());return 1;}}
