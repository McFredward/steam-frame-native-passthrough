#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include "stream-bridge-sender-provider-sourceipc.h"
#include <cwchar>
namespace stream_bridge::sender {
struct SourceIpcServer::Impl {
    Prepared prepared;void* context;HANDLE pipe=INVALID_HANDLE_VALUE;OVERLAPPED connect{},read{},write{};
    bool connecting=false,reading=false,writing=false,connected=false,cancelling=false,accepted=false;unsigned writes=0;
    SourceIpcMessage request{},reply{},previous{};SourceIpcRequest client{};SourceIpcPermission permission{};std::uint8_t unexpected=0;
    Impl(Prepared p,void* c):prepared(p),context(c){connect.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);read.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);write.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
    bool done(OVERLAPPED& op,DWORD& bytes){return GetOverlappedResult(pipe,&op,&bytes,FALSE)!=0;}
    bool pending(OVERLAPPED& op){DWORD bytes=0;if(done(op,bytes))return false;return GetLastError()==ERROR_IO_INCOMPLETE;}
    void cancel(){permission.reset();if(pipe==INVALID_HANDLE_VALUE)return;cancelling=true;CancelIoEx(pipe,nullptr);DisconnectNamedPipe(pipe);}
    void reap(){
        if(!cancelling)return;
        if((connecting&&pending(connect))||(reading&&pending(read))||(writing&&pending(write)))return;
        CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;connecting=reading=writing=connected=cancelling=accepted=false;writes=0;permission.reset();client={};request={};reply={};previous={};
    }
    bool create(){
        if(!connect.hEvent||!read.hEvent||!write.hEvent)return false;
        wchar_t name[96]{};std::swprintf(name,96,L"\\\\.\\pipe\\SFNP.StreamBridge.V1.%lu",GetCurrentProcessId());
        PSECURITY_DESCRIPTOR security=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)(A;;GA;;;SY)",SDDL_REVISION_1,&security,nullptr))return false;
        SECURITY_ATTRIBUTES attributes{sizeof attributes,security,FALSE};
        pipe=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,128,128,0,&attributes);LocalFree(security);
        if(pipe==INVALID_HANDLE_VALUE)return false;
        ResetEvent(connect.hEvent);connecting=true;
        if(ConnectNamedPipe(pipe,&connect))connecting=false;
        else{const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)connecting=false;else if(error!=ERROR_IO_PENDING){cancel();return false;}}
        connected=!connecting;return true;
    }
    bool beginRead(){
        ResetEvent(read.hEvent);DWORD bytes=0;reading=true;
        void* destination=accepted?static_cast<void*>(&unexpected):request.data();const DWORD count=accepted?1:128;
        if(ReadFile(pipe,destination,count,&bytes,&read))return true;
        if(GetLastError()==ERROR_IO_PENDING)return true;
        reading=false;cancel();return false;
    }
    void tick(const SourceIpcState& state){
        if(cancelling){reap();return;}
        if(pipe==INVALID_HANDLE_VALUE){create();return;}
        DWORD bytes=0;
        if(connecting){if(!done(connect,bytes)){if(GetLastError()!=ERROR_IO_INCOMPLETE)cancel();return;}connecting=false;connected=true;}
        if(!connected)return;
        if(reading){if(done(read,bytes)){
            reading=false;
            if(accepted||bytes!=128){cancel();return;}
            ULONG pid=0;if(!GetNamedPipeClientProcessId(pipe,&pid)||!readSourceRequest(request,pid,client)||!prepared){cancel();return;}
            accepted=true;
        }else if(GetLastError()!=ERROR_IO_INCOMPLETE){reading=false;cancel();return;}}
        if(!reading)beginRead(); // An extra byte or EOF revokes this connection.
        if(!accepted||cancelling)return;
        if(writing){if(done(write,bytes)){writing=false;if(bytes!=128){cancel();return;}}else{if(GetLastError()!=ERROR_IO_INCOMPLETE){writing=false;cancel();}return;}}
        const auto next=permission.update(client,GetCurrentProcessId(),state,prepared(client.processId,context));
        if(writes&&next==previous)return;
        if(writes>=256){cancel();return;} // Bounded connection telemetry, no timer.
        reply=next;previous=next;ResetEvent(write.hEvent);writing=true;++writes;
        if(!WriteFile(pipe,reply.data(),128,&bytes,&write)&&GetLastError()!=ERROR_IO_PENDING){writing=false;cancel();}
    }
};
SourceIpcServer::SourceIpcServer(Prepared p,void* context):impl_(new Impl(p,context)){}
SourceIpcServer::~SourceIpcServer(){stop();(void)impl_.release();} // Pending OS buffers stay alive through process exit.
void SourceIpcServer::tick(const SourceIpcState& state)noexcept{try{impl_->tick(state);}catch(...){impl_->cancel();}}
void SourceIpcServer::stop()noexcept{impl_->cancel();impl_->reap();}
} // namespace stream_bridge::sender
