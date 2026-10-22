#include "bridge/stream_bridge_log.h"
#include "stream_bridge_camera_client.h"
#include "../layer/pixel_snapshot_io.h"
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
namespace stream_bridge_camera {
namespace {
bool ownedFile(int parent,const char* name,std::size_t maximum){
    const int fd=openat(parent,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return false;
    struct stat info{};const bool good=!fstat(fd,&info)&&S_ISREG(info.st_mode)&&info.st_uid==getuid()
        &&(info.st_mode&07777)==0600&&info.st_nlink==1&&info.st_size>0&&std::uint64_t(info.st_size)<=maximum;
    close(fd);return good;
}
bool launchFiles(const std::filesystem::path& root){
    const auto text=root.string();const int directory=frame_pixel_io::privateDirectory(text);if(directory<0)return false;
    const int tools=openat(directory,"tools",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};const bool good=tools>=0&&!fstat(tools,&info)&&S_ISDIR(info.st_mode)&&info.st_uid==getuid()
        &&(info.st_mode&07777)==0700&&ownedFile(tools,"stream_bridge_camera.py",256*1024)
        &&ownedFile(directory,"camera-session.json",16*1024);
    if(tools>=0)close(tools);
    close(directory);return good;
}
bool omitEnvironment(const char* entry){
    return !std::strncmp(entry,"LD_PRELOAD=",11)||!std::strncmp(entry,"LD_AUDIT=",9)||!std::strncmp(entry,"LD_LIBRARY_PATH=",16)
        ||!std::strncmp(entry,"PYTHON",6)||!std::strncmp(entry,"SFNP_STREAM_BRIDGE_CONFIG=",24);
}
bool clockSample(ClockSample& sample,void*) noexcept{
    timespec boot{},monotonic{};if(clock_gettime(CLOCK_BOOTTIME,&boot)||clock_gettime(CLOCK_MONOTONIC,&monotonic)||boot.tv_sec<0||monotonic.tv_sec<0)return false;
    sample.bootNanoseconds=std::uint64_t(boot.tv_sec)*1000000000ULL+std::uint64_t(boot.tv_nsec);
    sample.monotonicNanoseconds=std::uint64_t(monotonic.tv_sec)*1000000000ULL+std::uint64_t(monotonic.tv_nsec);return true;
}
struct SpawnFiles {
    int pair[2]{-1,-1};posix_spawn_file_actions_t actions{};bool initialized=false;
    ~SpawnFiles(){if(initialized)posix_spawn_file_actions_destroy(&actions);for(auto value:pair)if(value>=0)close(value);}
};
}
struct Client::Impl {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"Render authorization must be a lock-free scalar");
    std::atomic<std::uint64_t> grant{0};int fd=-1;pid_t child=-1;
    bool wanted=false,connected=false,hello=false,closing=false,terminal=false,recoveryDemand=false,restoreBarrier=false,clockKnown=false,failed=false;
    ClockReader clock=clockSample;void* clockContext=nullptr;std::uint64_t sleepOffset=0;
    std::uint64_t sequence=0,request=0,sent=0,acknowledged=0;
    std::array<char,64> input{},output{};std::size_t inputBytes=0,outputBytes=0,outputAt=0;
    std::uint64_t outputSequence=0;
    std::chrono::steady_clock::time_point retry{};unsigned notes=0;
    void note(const char* text){if(stream_bridge::logging::enabled(stream_bridge::logging::Level::Warning)&&notes++<8)std::fprintf(stderr,"[Steam Link Native Passthrough] camera session: %s\n",text);}
    bool next(){
        grant.store(0,std::memory_order_release);
        if(sequence==~std::uint64_t(0)){terminal=true;return false;}
        request=++sequence;return true;
    }
    void set(bool value){if(value!=wanted){wanted=value;next();}if(!value)grant.store(0,std::memory_order_release);}
    void closeChannel(){grant.store(0,std::memory_order_release);if(fd>=0){close(fd);fd=-1;}hello=false;inputBytes=outputBytes=outputAt=0;sent=acknowledged=0;}
    void fail(const char* text){note(text);failed=recoveryDemand=true;closeChannel();retry=std::chrono::steady_clock::now()+std::chrono::seconds(1);}
    bool target()const{return wanted&&!restoreBarrier&&connected&&!closing;}
    void checkResume(){
        if(!connected&&child<0){clockKnown=false;return;}
        ClockSample sample{};if(!clock(sample,clockContext)){fail("Linux resume clock unavailable; camera readiness withdrawn");return;}
        const auto offset=sample.bootNanoseconds>sample.monotonicNanoseconds?sample.bootNanoseconds-sample.monotonicNanoseconds:0;
        if(clockKnown&&offset>sleepOffset&&offset-sleepOffset>5000000ULL){
            grant.store(0,std::memory_order_release);restoreBarrier=true;next();note("resume observed; restore acknowledgment required before native alpha renewal");
        }
        // Preemption between these two clock reads can only lower their
        // observed offset. Never adopt that lower sample as a new baseline:
        // its later recovery is CPU scheduling, not another OS suspend.
        if(!clockKnown||offset>sleepOffset)sleepOffset=offset;
        clockKnown=true;
    }
    void reap(){if(child>0){int result=0;const auto reaped=waitpid(child,&result,WNOHANG);if(reaped==child||(reaped<0&&errno==ECHILD)){const bool normal=!failed&&closing&&reaped==child&&WIFEXITED(result)&&!WEXITSTATUS(result);child=-1;closeChannel();retry=normal?std::chrono::steady_clock::time_point{}:std::chrono::steady_clock::now()+std::chrono::seconds(1);}}}
    bool spawn(const std::filesystem::path& root){
        if(!launchFiles(root)){fail("owned helper/configuration unavailable; native camera preference retained");return false;}
        SpawnFiles files;if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,files.pair)){fail("private helper channel unavailable");return false;}
        for(auto& fd:files.pair)if(fd<3){const int copy=fcntl(fd,F_DUPFD_CLOEXEC,3);close(fd);fd=copy;if(fd<0){fail("private helper descriptors unavailable");return false;}}
        if(posix_spawn_file_actions_init(&files.actions)){fail("helper file actions unavailable");return false;}files.initialized=true;
        int error=posix_spawn_file_actions_adddup2(&files.actions,files.pair[1],STDIN_FILENO);
        if(!error)error=posix_spawn_file_actions_adddup2(&files.actions,files.pair[1],STDOUT_FILENO);
        // Supported Frame glibc and the cross SDK expose closefrom. It runs in
        // the spawned child only, after its private stdin/stdout were installed.
        // A native non-CLOEXEC GPU/decoder/socket FD must never become ours.
        if(!error)error=posix_spawn_file_actions_addclosefrom_np(&files.actions,3);
        const auto directory=root.string(),script=(root/"tools/stream_bridge_camera.py").string();
        const char* values[]{"/usr/bin/python3","-I","-B",script.c_str(),"serve","--root",directory.c_str(),nullptr};
        std::vector<char*> environment;for(auto** old=environ;old&&*old;++old)if(!omitEnvironment(*old))environment.push_back(*old);environment.push_back(nullptr);
        if(!error)error=posix_spawn(&child,values[0],&files.actions,nullptr,const_cast<char* const*>(values),environment.data());
        if(error){child=-1;fail("background helper could not start");return false;}
        fd=files.pair[0];files.pair[0]=-1;const auto flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){fail("nonblocking helper channel unavailable");return false;}
        hello=closing=failed=false;inputBytes=outputBytes=outputAt=0;sent=acknowledged=0;next();return true;
    }
    bool line(const char* bytes,std::size_t count){
        if(count<3||bytes[1]!=' ')return false;
        std::uint64_t value=0;const auto parsed=std::from_chars(bytes+2,bytes+count,value);
        if(parsed.ec!=std::errc()||parsed.ptr!=bytes+count)return false;
        if(bytes[0]=='R'){if(value||hello)return false;hello=true;return true;}
        if(bytes[0]=='N'&&hello&&value&&value<=sequence){fail("native settings session lost; camera readiness withdrawn");return true;}
        if(!hello||(bytes[0]!='A'&&bytes[0]!='D')||!value||value>sequence)return false;
        // An older enable response may follow a newer disable request. It can
        // never republish readiness or acknowledge the wrong source session.
        if(value!=request||value!=sent)return true;
        if(bytes[0]=='A'&&target()){acknowledged=value;grant.store(value,std::memory_order_release);}
        else if(bytes[0]=='D'&&!target()){
            acknowledged=value;grant.store(0,std::memory_order_release);
            if(restoreBarrier){restoreBarrier=false;if(wanted&&connected&&!closing)next();}
        }
        else return false;
        return true;
    }
    void read(){
        for(unsigned batch=0;fd>=0&&batch<8;++batch){char bytes[128];const auto got=recv(fd,bytes,sizeof bytes,MSG_DONTWAIT);
            if(got<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return;
            if(got<=0){fail("helper ended; exact camera readiness revoked");return;}
            for(ssize_t n=0;n<got;++n){if(bytes[n]=='\n'){if(!line(input.data(),inputBytes)){fail("helper response refused");return;}inputBytes=0;if(fd<0)return;}
                else {if(inputBytes==input.size()){fail("oversized helper response refused");return;}input[inputBytes++]=bytes[n];}}
        }
    }
    void write(){
        if(!hello||fd<0||terminal)return;
        if(!outputBytes&&sent!=request){
            const auto count=std::snprintf(output.data(),output.size(),"%c %llu\n",target()?'E':'D',static_cast<unsigned long long>(request));
            if(count<=0||std::size_t(count)>=output.size()){fail("helper request refused");return;}
            outputBytes=std::size_t(count);outputAt=0;outputSequence=request;
        }
        if(!outputBytes)return;
        const auto done=send(fd,output.data()+outputAt,outputBytes-outputAt,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(done<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return;
        if(done<=0){fail("helper command could not be sent");return;}
        outputAt+=std::size_t(done);if(outputAt==outputBytes){sent=outputSequence;outputBytes=outputAt=0;}
    }
};
Client::Client(ClockReader clock,void* context):impl_(new Impl){if(clock){impl_->clock=clock;impl_->clockContext=context;}}
Client::~Client(){impl_->closeChannel();impl_->reap();}
void Client::request(bool enabled) noexcept{impl_->set(enabled);}
void Client::tickCold(const std::filesystem::path& root,bool peer) noexcept{
    auto&p=*impl_;try{
        if(peer!=p.connected){if(peer&&p.closing)p.closeChannel();p.connected=peer;p.grant.store(0,std::memory_order_release);p.next();if(!peer){p.set(false);p.closing=true;}}
        p.checkResume();
        p.reap();
        if(p.terminal){p.closeChannel();return;}
        if(p.child<0&&peer&&(!p.recoveryDemand||p.wanted)&&std::chrono::steady_clock::now()>=p.retry)p.spawn(root);
        if(p.fd<0)return;
        p.read();p.write();
        if(p.closing&&!p.wanted&&p.acknowledged==p.request&&p.sent==p.request)p.closeChannel();
    }catch(...){p.fail("cold helper operation refused; native camera preference preserved");}
}
void Client::stopCold() noexcept{auto&p=*impl_;p.set(false);p.connected=false;p.closing=true;p.closeChannel();p.reap();}
std::uint64_t Client::epoch()const noexcept{return impl_->grant.load(std::memory_order_acquire);}
bool Client::ready()const noexcept{return epoch()!=0;}
bool Client::runningCold()const noexcept{return impl_->child>0;}
}
