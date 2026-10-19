#include <cstdio>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef SFNP_PROCESS_SOURCE
#define SFNP_PROCESS_SOURCE "../../layer/stream_bridge_process.cpp"
#endif
#include SFNP_PROCESS_SOURCE
namespace renderer=stream_bridge_renderer;
namespace {
unsigned checks=0;
void check(bool valid,const char* message){++checks;if(!valid)throw std::runtime_error(message);}
struct Directory {
    std::filesystem::path path;
    Directory(){char name[]="/tmp/sfnp-process712-XXXXXX";check(mkdtemp(name)!=nullptr,"private fixture directory created");path=name;}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
struct File {
    int fd=-1;File(){fd=memfd_create("process-fixture",MFD_CLOEXEC);check(fd>=0&&ftruncate(fd,4096)==0,"actual owned image allocation");}
    ~File(){if(fd>=0)close(fd);}
};
local::PeerPin selfPin(){
    FILE* file=fopen("/proc/self/exe","rb");check(file!=nullptr,"read exact fixture peer executable");
    frame_receiver::Sha256 sha;std::array<unsigned char,16384> bytes{};std::size_t total=0,n=0;
    while((n=fread(bytes.data(),1,bytes.size(),file))){sha.update(bytes.data(),n);total+=n;}
    fclose(file);return {total,sha.finish()};
}
void connect(renderer::Process& p,local::Channel& producer,const Directory& directory){
    const auto pin=selfPin();p.kind=renderer::ProcessKind::Compositor;p.root=directory.path;
    p.connection=std::make_shared<std::atomic<bool>>(true);
    const auto path=p.root/"receiver.sock";
    check(p.channel.preparePinned(local::Endpoint::CompositorConsumer,path,pin),"real checked consumer socket prepared");
    check(producer.preparePinned(local::Endpoint::VrlinkProducer,path,pin),"real checked producer socket prepared");
    for(unsigned n=0;n<80;++n){p.channel.tick();producer.tick();}
    check(p.channel.state()==local::State::Connected&&producer.state()==local::State::Connected,"both actual channels authenticated by SO_PEERCRED and executable pin");
}
void noConsumers(){
    Directory directory;renderer::Process p;local::Channel producer;connect(p,producer,directory);
    const auto connection=p.connection;local::State previous=local::State::Connected;
    check(renderer::imageRoutePrepared(&p),"connected route is initially visible to decoder worker");
    renderer::workCycle(p,previous);
    check(!connection->load(std::memory_order_acquire),"last consumer disappearance immediately revokes old picture connection");
    check(p.channel.state()==local::State::Off&&previous==local::State::Off,"actual worker cycle closes channel when no consumer exists");
    check(!renderer::imageRoutePrepared(&p),"missing consumer withdraws producer route admission");
    check(!std::filesystem::exists(directory.path/"receiver.sock"),"worker removes only its owned consumer socket");
    local::Message message;message.source.stream.connection.fill(1);message.source.stream.generation=message.source.outputPoolGeneration=1;
    message.descriptor={1,1,1,1};message.binding.fill(2);message.submittedContent=1;message.peerConfirmed=true;message.certifiedMetadata={1};
    check(producer.enqueue(message),"producer attempts its next real metadata send after peer teardown");
    producer.tick();check(producer.state()==local::State::Lost,"actual next producer send detects the closed route");
}
struct Consumer {bool ready=false;unsigned ticks=0;};
bool admitted(void* pointer) noexcept{return static_cast<Consumer*>(pointer)->ready;}
void tick(void* pointer) noexcept{++static_cast<Consumer*>(pointer)->ticks;}
void consumerAdmission(){
    Directory directory;renderer::Process p;local::Channel producer;connect(p,producer,directory);
    Consumer consumer;p.consumers[0]={&consumer,nullptr,nullptr,tick,admitted};local::State previous=local::State::Connected;
    auto old=p.connection;renderer::workCycle(p,previous);
    check(p.channel.state()==local::State::Off&&!old->load(std::memory_order_acquire)&&consumer.ticks==0,"present but unadmitted consumer cannot keep image route active");
    producer.stop();connect(p,producer,directory);old=p.connection;consumer.ready=true;previous=local::State::Connected;
    renderer::workCycle(p,previous);
    check(p.channel.state()==local::State::Connected&&old->load(std::memory_order_acquire),"admitted real consumer retains native connected transport");
    check(consumer.ticks==1&&renderer::imageRoutePrepared(&p),"only admitted consumer receives worker tick and grants producer route");
    consumer.ready=false;renderer::workCycle(p,previous);
    check(!old->load(std::memory_order_acquire)&&p.channel.state()==local::State::Off&&consumer.ticks==1,"revoked consumer cannot retain connection or receive another callback");
}
void sourceLossReconnect(){
    renderer::Process p;File file;check(p.writer.prepareCold()&&p.writer.associateResourceFd(file.fd),"process writer prepared with actual resource identity");
    native::CaptureObservation capture{native::CaptureKind::Decoded,23,file.fd,2,71,7,21,1};
    p.writer.captureEvent(capture);local::LeaseReference old;
    check(p.writer.reference(capture,old),"actual source capture has first-epoch reference");
    const int fd=p.writer.duplicateReadOnly();local::LeaseReader before;
    check(fd>=0&&before.mapReadOnly(fd)&&before.current(old),"existing consumer sees current exact source lease");
    native::RuntimeNotice notice;notice.state=native::RuntimeState::Negotiating;renderer::sourceNotice(notice,&p);
    check(before.current(old),"unrelated negotiating notice does not revoke a valid source lease");
    notice.state=native::RuntimeState::SenderUnavailable;renderer::sourceNotice(notice,&p);
    check(!before.current(old),"real SenderUnavailable callback invalidates old source reader immediately");
    ++capture.contentGeneration;p.writer.captureEvent(capture);local::LeaseReference fresh;
    check(p.writer.reference(capture,fresh)&&fresh.epoch>old.epoch,"source callback uses nonterminal epoch and permits actual new capture");
    local::LeaseReader after;check(after.mapReadOnly(fd)&&after.current(fresh)&&!after.current(old)&&!before.current(fresh),"fresh reconnect reader succeeds without reviving any old reference or mapping");
    notice.state=native::RuntimeState::SourceLost;renderer::sourceNotice(notice,&p);
    check(!after.current(fresh),"real SourceLost callback also invalidates current source epoch");
    local::LeaseReference next;check(p.writer.reference(capture,next)&&next.epoch>fresh.epoch,"successive source losses still allow a future admission epoch");
    close(fd);
}
void telemetry(){
    renderer::Process p;p.kind=renderer::ProcessKind::Compositor;
    native::CertifiedFrame callback;renderer::certified(callback,&p);
    check(p.flow[unsigned(renderer::Flow::CertifiedCallback)]==1&&p.flow[unsigned(renderer::Flow::CertifiedQueued)]==0,
        "an unconfirmed certificate callback never counts as locally queued metadata");
    native::DecodedFrame decoded;renderer::decoded(decoded,&p);
    check(p.flow[unsigned(renderer::Flow::DecodedCallback)]==1&&p.flow[unsigned(renderer::Flow::DecodedQueued)]==0,
        "an unconfirmed decoded callback never counts as a queued picture");
    local::Message invalid;invalid.peerConfirmed=true;invalid.kind=local::Kind::Certified;
    renderer::consume(p,invalid);
    check(p.flow[unsigned(renderer::Flow::CertifiedReceived)]==1&&p.flow[unsigned(renderer::Flow::AuthenticationRefused)]==1&&p.flow[unsigned(renderer::Flow::Authenticated)]==0,
        "invalid detached metadata is separately counted without image authority");
    invalid.kind=local::Kind::Decoded;renderer::consume(p,invalid);
    check(p.flow[unsigned(renderer::Flow::DecodedReceived)]==1&&p.flow[unsigned(renderer::Flow::CertificateMiss)]==1&&p.flow[unsigned(renderer::Flow::PicturesRetained)]==0,
        "decoded packet without exact certificate never counts as a retained local picture");
    renderer::reportFlowCold(p);const auto first=p.flowLines;
    for(unsigned i=0;i<4096;++i)renderer::reportFlowCold(p);
    check(first==1&&p.flowLines==first,"unchanged local flow never repeats cold status");
    for(unsigned bit=1;bit<64;++bit){p.flow[unsigned(renderer::Flow::CertifiedCallback)]=std::uint64_t(1)<<bit;renderer::reportFlowCold(p);}
    check(p.flowLines==48,"changed local status has an absolute per-process line bound");
}
}
int main(){try{noConsumers();consumerAdmission();sourceLossReconnect();telemetry();std::printf("%u actual process lifecycle checks passed\n",checks);return 0;}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
