#include "receiver/stream_bridge_local.h"
#include "receiver/sha256.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
namespace local=stream_bridge_local;namespace native=stream_bridge_native;
namespace {
unsigned checks=0;void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Directory{std::filesystem::path path;Directory(){char n[]="/tmp/sfnp-availability-XXXXXX";check(mkdtemp(n)!=nullptr,"owned fixture directory");path=n;}~Directory(){std::error_code e;std::filesystem::remove_all(path,e);}};
local::PeerPin pin(){FILE* f=fopen("/proc/self/exe","rb");check(f!=nullptr,"actual checked peer binary");frame_receiver::Sha256 hash;std::array<unsigned char,16384> bytes{};std::size_t total=0,n=0;while((n=fread(bytes.data(),1,bytes.size(),f))){hash.update(bytes.data(),n);total+=n;}fclose(f);return {total,hash.finish()};}
}
int main(){try{
 Directory dir;local::Channel producer,consumer;const auto peer=pin();
 check(consumer.preparePinned(local::Endpoint::CompositorConsumer,dir.path/"receiver.sock",peer)&&producer.preparePinned(local::Endpoint::VrlinkProducer,dir.path/"receiver.sock",peer),"real same-uid executable-pinned socket endpoints");
 for(unsigned n=0;n<8;++n){consumer.tick();producer.tick();}
 check(consumer.state()==local::State::Connected&&producer.state()==local::State::Connected,"actual peer authenticated");
 const int image=memfd_create("availability-image",MFD_CLOEXEC);check(image>=0&&ftruncate(image,4096)==0,"actual source image fd");
 local::LeaseWriter writer;check(writer.prepareCold()&&writer.associateResourceFd(image),"real lease publication prepared");
 native::CaptureObservation capture{native::CaptureKind::Decoded,23,image,2,71,7,21,1};writer.captureEvent(capture);local::LeaseReference reference;check(writer.reference(capture,reference),"exact decoded capture owns a shared lease");
 local::Message cert;cert.kind=local::Kind::Certified;cert.source.stream.connection.fill(1);cert.source.stream.generation=cert.source.outputPoolGeneration=7;cert.descriptor={1000,41,16,2};cert.binding.fill(3);cert.nativeTimestampToken=71;cert.submittedContent=6;cert.peerConfirmed=true;cert.certifiedMetadata={1};
 auto decoded=cert;decoded.kind=local::Kind::Decoded;decoded.certifiedMetadata.clear();decoded.lease=reference;decoded.dmabuf=image;decoded.readOnlyLeases=writer.duplicateReadOnly();
 check(producer.enqueue(cert)&&producer.enqueue(decoded),"certificate then exact decoded packet queued together");close(decoded.readOnlyLeases);
 producer.tick();consumer.tick();local::Message first,second;
 check(consumer.receive(first)&&first.kind==local::Kind::Certified,"first production tick publishes certificate");
 const bool sameCycle=consumer.receive(second);
 std::printf("channel: decoded-visible-after-one-tick=%u; receive-loop cannot advance framing without another tick\n",unsigned(sameCycle));
 check(!sameCycle,"current channel tick stops after exactly one ready packet");
 consumer.tick();check(consumer.receive(second)&&second.kind==local::Kind::Decoded,"second exact packet appears only after another production tick");
 local::LeaseReader reader;check(reader.mapReadOnly(second.readOnlyLeases)&&reader.current(second.lease),"delayed packet remains exact, no stale/latest substitution");
 close(second.dmabuf);close(second.readOnlyLeases);close(image);
 std::printf("PASS: %u real Unix socket/SCM_RIGHTS/lease timing checks\n",checks);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
