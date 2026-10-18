#include "receiver/stream_bridge_local.h"
#include "receiver/sha256.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
namespace local=stream_bridge_local;namespace nd=stream_bridge_native;
namespace {
unsigned checks=0;void check(bool v,const char* n){++checks;if(!v)throw std::runtime_error(n);}
struct File {int fd=-1;File(){fd=memfd_create("fixture",MFD_CLOEXEC|MFD_ALLOW_SEALING);check(fd>=0&&ftruncate(fd,4096)==0,"actual owned allocation fd");}~File(){if(fd>=0)close(fd);}};
struct Directory {std::filesystem::path path;Directory(){char name[]="/tmp/sfnp-local712-XXXXXX";check(mkdtemp(name)!=nullptr,"actual private socket directory");path=name;}~Directory(){std::error_code e;std::filesystem::remove_all(path,e);}};
nd::CaptureObservation capture(int fd,std::uint64_t content=1){return {nd::CaptureKind::Decoded,23,fd,2,71,7,21,content};}
local::PeerPin selfPin(){FILE* file=fopen("/proc/self/exe","rb");check(file!=nullptr,"actual peer executable fixture");frame_receiver::Sha256 sha;std::array<std::uint8_t,16384> buffer{};std::size_t size=0,n=0;while((n=fread(buffer.data(),1,buffer.size(),file))){sha.update(buffer.data(),n);size+=n;}fclose(file);return {size,sha.finish()};}
void pump(local::Channel& source,local::Channel& target){for(unsigned i=0;i<80;++i){target.tick();source.tick();}}
local::Message certified(){local::Message m;m.kind=local::Kind::Certified;m.source.stream.connection.fill(1);m.source.stream.generation=m.source.outputPoolGeneration=7;
    m.descriptor={1000,41,2048,2048};m.binding.fill(3);m.nativeTimestampToken=71;m.submittedContent=6;m.peerConfirmed=true;m.certifiedMetadata={0,0,0,1,0x26,1,0x80};return m;}
void sharedLeases(){File file;local::LeaseWriter writer;check(writer.prepareCold()&&writer.associateResourceFd(file.fd),"cold own shared lease and resource identities");
    const auto c=capture(file.fd);writer.captureEvent(c);local::LeaseReference ref{};check(writer.reference(c,ref)&&ref.sequence&&ref.kernel[1],"actual decoded event owns exact atomic lease reference");
    const int fd=writer.duplicateReadOnly();check(fd>=0&&(fcntl(fd,F_GETFL)&O_ACCMODE)==O_RDONLY,"lease descriptor is genuinely read-only");
    local::LeaseReader reader;check(reader.mapReadOnly(fd)&&reader.current(ref),"actual shared memfd mapping validates source lease");
    check(pwrite(fd,"x",1,0)==-1,"receiver cannot write shared lease fd");
    auto wrong=ref;++wrong.capture.contentGeneration;check(!reader.current(wrong),"shared current check binds exact content rather than latest slot");
    auto next=c;next.kind=nd::CaptureKind::Requeue;writer.captureEvent(next);check(!reader.current(ref)&&!writer.reference(c,wrong),"synchronous pre-QBUF event invalidates independently of socket/worker timing");
    const auto c2=capture(file.fd,2);writer.captureEvent(c2);check(writer.reference(c2,wrong)&&reader.current(wrong)&&wrong.sequence!=ref.sequence&&!reader.current(ref),"slot reuse increments stable atomic sequence");
    next.kind=nd::CaptureKind::SourceReset;writer.captureEvent(next);check(!reader.current(wrong),"actual source reset invalidates all its capture slots");
    writer.captureEvent(c2);check(writer.reference(c2,wrong),"new source fact can reestablish lease");writer.revoke();check(!reader.current(wrong),"explicit source revocation immediately reaches read-only consumer");close(fd);
    local::LeaseReader bad;check(!bad.mapReadOnly(file.fd),"unsealed writable arbitrary memory is not source lease authority");
}
void separateProcessLease(){File file;local::LeaseWriter writer;check(writer.prepareCold()&&writer.associateResourceFd(file.fd),"separate-process source prepared");
    const auto c=capture(file.fd);writer.captureEvent(c);local::LeaseReference ref;check(writer.reference(c,ref),"separate-process source reference");
    const int fd=writer.duplicateReadOnly();int signal[2]{},result[2]{};check(fd>=0&&pipe(signal)==0&&pipe(result)==0,"explicit child barriers");
    const auto child=fork();check(child>=0,"actual separate reader process");
    if(child==0){close(signal[1]);close(result[0]);local::LeaseReader reader;char state=reader.mapReadOnly(fd)&&reader.current(ref)?1:0;
        if(write(result[1],&state,1)!=1){_exit(2);}
        char trigger=0;if(read(signal[0],&trigger,1)!=1){_exit(3);}
        state=reader.current(ref)?0:1;if(write(result[1],&state,1)!=1)_exit(4);_exit(0);}
    close(signal[0]);close(result[1]);char state=0;check(read(result[0],&state,1)==1&&state==1,"child mapped real read-only current lease");
    auto q=c;q.kind=nd::CaptureKind::Requeue;writer.captureEvent(q);check(write(signal[1],"x",1)==1,"source invalidation happened before child check");
    check(read(result[0],&state,1)==1&&state==1,"different process sees immediate shared invalidation without receiving a new frame");
    int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"separate reader process reaped");close(signal[1]);close(result[0]);close(fd);
}
void localFramingAndRights(){Directory dir;local::Channel source,target;const auto pin=selfPin();const auto path=dir.path/"receiver.sock";
    check(target.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"actual private consumer listens");
    check(source.preparePinned(local::Endpoint::VrlinkProducer,path,pin),"actual private producer connects");pump(source,target);
    check(source.state()==local::State::Connected&&target.state()==local::State::Connected,"actual SO_PEERCRED and both peer exe hashes checked");
    auto c=certified();c.source.stream.generation=42;c.certifiedMetadata.resize(800000,0x55);check(source.enqueue(c),"bounded actual large certificate queued");pump(source,target);
    local::Message got;check(target.receive(got)&&got.certifiedMetadata==c.certifiedMetadata&&got.descriptor.sourceFrame==c.descriptor.sourceFrame
        &&got.binding==c.binding&&got.source.stream.generation==42&&got.source.outputPoolGeneration==7&&got.dmabuf<0&&got.readOnlyLeases<0,"partial stream header/payload keeps admission epoch distinct from actual native pool");
    File image;local::LeaseWriter writer;check(writer.prepareCold()&&writer.associateResourceFd(image.fd),"source shared leases prepared");const auto captureFact=capture(image.fd);
    writer.captureEvent(captureFact);local::LeaseReference ref;check(writer.reference(captureFact,ref),"source matching decoded lease obtained");
    local::Message decoded=c;decoded.kind=local::Kind::Decoded;decoded.certifiedMetadata.clear();decoded.lease=ref;decoded.dmabuf=image.fd;decoded.readOnlyLeases=writer.duplicateReadOnly();
    check(source.enqueue(decoded),"SCM_RIGHTS decoded lease queued as owned duplicates");close(decoded.readOnlyLeases);decoded.readOnlyLeases=-1;pump(source,target);
    check(target.receive(got)&&got.kind==local::Kind::Decoded&&got.dmabuf>=0&&got.readOnlyLeases>=0&&got.dmabuf!=image.fd,"actual SCM_RIGHTS transfers owned descriptors, not producer fd integers");
    struct stat a{},b{};check(fstat(image.fd,&a)==0&&fstat(got.dmabuf,&b)==0&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino,"transferred DMA allocation preserves exact kernel identity");
    local::LeaseReader reader;check(reader.mapReadOnly(got.readOnlyLeases)&&reader.current(got.lease),"actual transferred shared lease remains tied to current source capture");
    auto requeue=captureFact;requeue.kind=nd::CaptureKind::Requeue;writer.captureEvent(requeue);check(!reader.current(got.lease),"cross-process-design shared mapping sees immediate source invalidation without IPC receive");
    close(got.dmabuf);close(got.readOnlyLeases);source.stop();pump(source,target);check(target.state()==local::State::Lost,"actual socket EOF revokes local channel");
    target.stop();check(!std::filesystem::exists(path),"only owned unchanged socket removed on stop");
}
void peerAndPathRefusals(){Directory dir;const auto path=dir.path/"receiver.sock";local::Channel server,client;const auto pin=selfPin();
    check(server.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"refusal listener");
    check(client.prepare(local::Endpoint::VrlinkProducer,path),"production client transport starts but waits for actual compositor identity");pump(client,server);
    check(client.state()==local::State::Refused,"actual fixture process cannot impersonate production compositor hash");client.stop();server.stop();
    check(server.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"second listener");auto wrong=pin;wrong.sha256[0]^=1;
    check(client.preparePinned(local::Endpoint::VrlinkProducer,path,wrong),"wrong peer fixture connects");pump(client,server);check(client.state()==local::State::Refused,"wrong actual peer executable hash refused");server.stop();client.stop();
    const auto foreign=dir.path/"foreign";FILE* f=fopen(foreign.c_str(),"wb");check(f!=nullptr,"foreign file fixture");fputs("preserve",f);fclose(f);
    check(!server.preparePinned(local::Endpoint::CompositorConsumer,foreign,pin)&&std::filesystem::file_size(foreign)==8,"existing unknown path never unlinked/overwritten");
    chmod(dir.path.c_str(),0755);check(!server.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"public socket directory refused");chmod(dir.path.c_str(),0700);
    check(server.preparePinned(local::Endpoint::CompositorConsumer,path,pin)&&client.preparePinned(local::Endpoint::VrlinkProducer,path,pin),"capacity endpoints");pump(client,server);auto m=certified();
    check(client.enqueue(m)&&client.enqueue(m)&&!client.enqueue(m),"two-frame local queue bounded without eviction");
    auto bad=m;bad.certifiedMetadata.resize(local::Channel::MaximumPayload+1);check(!client.enqueue(bad),"oversized local payload refused");
}
int rawConnect(const std::filesystem::path& path){const int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);check(fd>=0,"actual malformed-input source socket");sockaddr_un address{};address.sun_family=AF_UNIX;std::strcpy(address.sun_path,path.c_str());check(connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address)==0,"actual malformed-input source connects");return fd;}
unsigned fdCount(){unsigned n=0;for(int fd=0;fd<512;++fd)if(fcntl(fd,F_GETFD)>=0)++n;return n;}
void malformedAndForeignLifetime(){Directory dir;const auto path=dir.path/"receiver.sock";const auto pin=selfPin();local::Channel server;
    check(server.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"malformed-input listener");int fd=rawConnect(path);server.tick();
    std::array<std::uint8_t,192> bad{};std::memcpy(bad.data(),"SFL1",4);bad[4]=2;
    check(send(fd,bad.data(),bad.size()-1,MSG_NOSIGNAL)==static_cast<ssize_t>(bad.size()-1),"partial untrusted header source");server.tick();
    local::Message out;check(server.state()==local::State::Connected&&!server.receive(out),"partial header never publishes metadata");
    check(send(fd,bad.data()+bad.size()-1,1,MSG_NOSIGNAL)==1,"complete bad version header");server.tick();check(server.state()==local::State::Refused,"unsupported local version refused before payload allocation");close(fd);server.stop();
    check(server.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"extra-rights listener");fd=rawConnect(path);server.tick();File f1,f2,f3;const auto before=fdCount();
    iovec io{bad.data(),bad.size()};msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;alignas(cmsghdr)std::array<char,CMSG_SPACE(sizeof(int)*3)> rights{};
    message.msg_control=rights.data();message.msg_controllen=rights.size();auto* control=CMSG_FIRSTHDR(&message);control->cmsg_level=SOL_SOCKET;control->cmsg_type=SCM_RIGHTS;control->cmsg_len=CMSG_LEN(sizeof(int)*3);int fds[]{f1.fd,f2.fd,f3.fd};std::memcpy(CMSG_DATA(control),fds,sizeof fds);
    check(sendmsg(fd,&message,MSG_NOSIGNAL)==static_cast<ssize_t>(bad.size()),"actual unexpected descriptor count transferred");server.tick();
    check(server.state()==local::State::Refused&&fdCount()<=before,"malformed SCM_RIGHTS descriptors are closed, not leaked");close(fd);server.stop();
    check(server.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"owned socket lifetime fixture");check(unlink(path.c_str())==0,"other actor replaced socket pathname");
    FILE* foreign=fopen(path.c_str(),"wb");check(foreign!=nullptr,"replacement file fixture");fputs("retain",foreign);fclose(foreign);server.stop();
    check(std::filesystem::is_regular_file(path)&&std::filesystem::file_size(path)==6,"shutdown never deletes another actor's replacement file");
}
}
int main(){try{sharedLeases();separateProcessLease();localFramingAndRights();peerAndPathRefusals();malformedAndForeignLifetime();std::printf("actual local transport/shared leases: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
