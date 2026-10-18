#include "../bridge/stream_bridge_control.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
// Test-only libc interposition makes the selection replacement occur exactly
// during the real credential read; production has no timing/test callback.
namespace {bool replaceSelection=false,selectionReplaced=false;std::array<char,4096> selectionPath{},retiredPath{};}
extern "C" ssize_t read(int fd,void* buffer,std::size_t length){
    if(replaceSelection&&length>128){
        replaceSelection=false;
        constexpr char replacement[]={"{\"schema\":\"sfnp-active-pairing-v1\",\"pair_id\":\"0102030405060708090a0b0c0d0e0f10\"}"};
        if(::rename(selectionPath.data(),retiredPath.data())==0){
            const int fresh=::open(selectionPath.data(),O_CREAT|O_EXCL|O_WRONLY,0600);
            if(fresh>=0){selectionReplaced=::write(fresh,replacement,sizeof(replacement)-1)==static_cast<ssize_t>(sizeof(replacement)-1);::close(fresh);}
        }
    }
    return static_cast<ssize_t>(syscall(SYS_read,fd,buffer,length));
}
namespace sb=stream_bridge;namespace rx=stream_bridge_receiver;namespace tx=stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool v,const char* name){++checks;if(!v)throw std::runtime_error(name);}
template<std::size_t N>sb::Bytes bytes(const std::array<std::uint8_t,N>& b){return {b.data(),b.size()};}
sb::Bytes bytes(const std::vector<std::uint8_t>& b){return {b.data(),b.size()};}
sb::Nonce nonce(unsigned base){sb::Nonce n{};for(unsigned i=0;i<16;++i)n[i]=static_cast<std::uint8_t>(base+i);return n;}
const char* pairId="0102030405060708090a0b0c0d0e0f10";
const char* key="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
const char* active="{\"schema\":\"sfnp-active-pairing-v1\",\"pair_id\":\"0102030405060708090a0b0c0d0e0f10\"}";
std::string credential(const char* role){return std::string("{\"schema\":\"sfnp-pairing-v1\",\"pair_id\":\"")+pairId+"\",\"key_hex\":\""+key+"\",\"role\":\""+role+"\",\"protocol_major\":1}";}
void write(const std::filesystem::path& path,const std::string& data){int fd=open(path.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW,0600);check(fd>=0,"fixture private file open");const auto n=::write(fd,data.data(),data.size());close(fd);check(n==static_cast<ssize_t>(data.size()),"fixture exact file write");check(chmod(path.c_str(),0600)==0,"fixture private file mode");}
struct Files{
    std::filesystem::path root,producer,receiver;
    Files(){std::array<char,80> path{};std::strcpy(path.data(),"/tmp/sfnp-control711-XXXXXX");const auto* result=mkdtemp(path.data());check(result!=nullptr,"private fixture directory");root=result;producer=root/"producer";receiver=root/"receiver";check(mkdir(producer.c_str(),0700)==0&&mkdir(receiver.c_str(),0700)==0,"private role directories");restore();}
    void restore(){write(producer/"active.json",active);write(receiver/"active.json",active);write(producer/(std::string(pairId)+".json"),credential("producer"));write(receiver/(std::string(pairId)+".json"),credential("receiver"));}
    ~Files(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}
};
sb::ControlRequest request(const std::filesystem::path& path,sb::Role role){sb::ControlRequest r;r.explicitlyEnabled=true;r.role=role;r.buildId=711;r.pairingsDirectory=path;std::strcpy(r.address.data(),"127.0.0.1");r.video={nonce(90),5};return r;}
sb::AuthPayload wire(const sb::Message& m){sb::AuthPayload p{};check(sb::encode(m,p)==sb::Status::Ok,"actual protocol encode");return p;}
tx::VerifiedSource source(){tx::VerifiedSource s;s.verified=s.ordinaryStream=s.completeAccessUnit=true;s.codec=tx::VerifiedSource::Codec::HevcAnnexB;s.moduleSha256[0]=1;s.apiVersion=0x7102000b;s.encoderGeneration=9;return s;}
const std::vector<std::uint8_t> ordinary{0,0,0,1,0x26,1,0x80,0x11,0x22};
void pump(sb::ControlRuntime& p,sb::ControlRuntime& r){for(unsigned i=0;i<80;++i){r.tick();p.tick();}}
void credentials(){
    Files files;sb::Pairing pairing;pairing.key.fill(0xa7);
    check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::Ok&&pairing.key[0]==1&&pairing.key[31]==32&&pairing.pairId==nonce(1),"actual selected private credential decoded");
    pairing.key.fill(0xa7);auto old=pairing;
    chmod((files.producer/(std::string(pairId)+".json")).c_str(),0644);
    check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::InsecureFile&&pairing.key==old.key,"broad file permission refused transactionally");files.restore();
    check(sb::loadPrivatePairing(files.producer,sb::Role::Receiver,pairing)==sb::CredentialStatus::RoleMismatch&&pairing.key==old.key,"wrong local role cannot consume secret");
    for(const auto& bad:{std::string("{}"),std::string("{\"schema\":\"sfnp-active-pairing-v1\",\"pair_id\":\"../old\"}"),std::string(active)+"x",std::string("{\"schema\":\"sfnp-active-pairing-v1\",\"schema\":\"sfnp-active-pairing-v1\",\"pair_id\":\"0102030405060708090a0b0c0d0e0f10\"}")}){
        write(files.producer/"active.json",bad);check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::MalformedSelection&&pairing.key==old.key,"invalid/duplicate/path/trailing selection cannot change output");}
    files.restore();const auto name=files.producer/(std::string(pairId)+".json");
    std::filesystem::rename(name,files.producer/"old.json");
    check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::MissingCredential,"loader never searches retained old keys");
    check(symlink("old.json",name.c_str())==0,"fixture credential symlink");check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::InsecureFile,"credential symlink refused");unlink(name.c_str());
    check(link((files.producer/"old.json").c_str(),name.c_str())==0,"fixture credential hardlink");check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::InsecureFile,"credential hardlink refused");unlink(name.c_str());unlink((files.producer/"old.json").c_str());files.restore();
    const auto redirect=files.root/"redirect";check(symlink(files.producer.c_str(),redirect.c_str())==0,"fixture directory redirect");
    check(sb::loadPrivatePairing(redirect,sb::Role::Producer,pairing)==sb::CredentialStatus::InsecureFile,"pairing directory symlink refused");
    check(sb::loadPrivatePairing(redirect/".."/"producer",sb::Role::Producer,pairing)==sb::CredentialStatus::InsecureFile,"intermediate symlink cannot be hidden by dotdot");
    chmod(files.producer.c_str(),0755);check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::InsecureFile,"public pairing directory refused");chmod(files.producer.c_str(),0700);
    auto wrong=credential("producer");wrong.back()=' ';wrong+=" ,\"unknown\":1}";write(name,wrong);check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::MalformedCredential,"unknown credential fields refused");
    auto version=credential("producer");version.replace(version.find("major\":1")+7,1,"2");write(name,version);check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::UnsupportedProtocol,"wrong credential protocol refused");
    files.restore();std::strncpy(selectionPath.data(),(files.producer/"active.json").c_str(),selectionPath.size()-1);std::strncpy(retiredPath.data(),(files.producer/"retired-active.json").c_str(),retiredPath.size()-1);
    replaceSelection=true;selectionReplaced=false;
    check(sb::loadPrivatePairing(files.producer,sb::Role::Producer,pairing)==sb::CredentialStatus::IoError&&selectionReplaced&&pairing.key==old.key,"same-content active selection replacement during credential read refuses publication");
    sb::Nonce a{},b{};check(sb::bridgeSystemRandom(a.data(),a.size())&&sb::bridgeSystemRandom(b.data(),b.size())&&a!=b&&a!=sb::Nonce{},"actual OS CSPRNG supplies fresh session nonces");check(!sb::bridgeSystemRandom(nullptr,16)&&!sb::bridgeSystemRandom(a.data(),65),"random input bounds");
}
void defaultsAndAddress(){
    sb::ControlRuntime runtime;sb::ControlRequest r;r.pairingsDirectory="/does-not-exist";
    check(runtime.prepare(r)==sb::ControlStatus::Disabled&&runtime.localPort()==0&&!runtime.peerVerified(),"default-off cold path opens no listener or pairing");
    r.explicitlyEnabled=true;r.buildId=711;
    for(const auto* address:{"0.0.0.0","255.255.255.255","224.0.0.1","8.8.8.8","localhost"}){std::strcpy(r.address.data(),address);check(runtime.prepare(r)==sb::ControlStatus::AddressRefused,"unspecified/public/multicast/DNS endpoints refused before credentials");}
    std::strcpy(r.address.data(),"127.0.0.1");check(runtime.prepare(r)==sb::ControlStatus::PairingRefused&&runtime.credentialStatus()==sb::CredentialStatus::MissingSelection,"missing private pairing gives bounded useful status");
}
void capabilities(){
    Files files;sb::ControlRuntime runtime;auto req=request(files.producer,sb::Role::Producer);
    req.offered=req.required=sb::KnownCapabilities;
    check(runtime.prepare(req)==sb::ControlStatus::Listening,"actual full image capabilities configured");
    sb::LocalConfig local;check(runtime.configuredLocal(local)&&local.offered==sb::KnownCapabilities&&local.required==sb::KnownCapabilities,"capabilities retained through real configuration");
    req.offered|=32;check(runtime.prepare(req)==sb::ControlStatus::AddressRefused,"unknown runtime capability refused");
    req.offered=sb::RequiredCapabilities;req.required=sb::KnownCapabilities;
    check(runtime.prepare(req)==sb::ControlStatus::AddressRefused,"required capability must be offered");
}
void loopbackHandshake(){
    Files files;sb::ControlRuntime p,r;auto rp=request(files.producer,sb::Role::Producer);
    check(p.prepare(rp)==sb::ControlStatus::Listening&&p.localPort()!=0,"actual nonblocking producer listener prepared from private key");
    auto rr=request(files.receiver,sb::Role::Receiver);rr.port=p.localPort();check(r.prepare(rr)==sb::ControlStatus::Connecting||r.status()==sb::ControlStatus::WaitingVideo,"actual receiver socket connect started");pump(p,r);
    check(!p.peerVerified()&&!r.peerVerified()&&p.status()==sb::ControlStatus::TransportConnected,"real TCP connection alone is never authenticated");
    sb::LocalConfig pc,rc;sb::LocalSendAuthority pa,ra;
    check(p.localConfig(pc,pa)&&r.configuredLocal(rc)&&!r.localConfig(rc,ra),"producer local protection and receiver configuration distinct from bootstrap");
    tx::SenderAdapter sender;rx::Receiver receiver;rx::Authority authority{rr.video.connection,rr.video.generation,{}};
    check(sender.request(pc,pa.channel,source(),p.senderSealer())==tx::SenderStatus::WaitingPeer,"actual runtime sender callback installed in existing adapter");
    check(receiver.beginAuthenticated(authority,rc,r.receiverVerifier(),r.verifierContext()),"actual runtime video verifier installed in existing receiver");
    sb::Message first;first.role=sb::Role::Receiver;first.buildId=rc.buildId;first.senderNonce=rc.nonce;auto firstWire=wire(first);std::uint64_t refused=77;
    check(!r.queueControl(bytes(firstWire),refused)&&refused==77,"TCP receiver cannot originate protected control before actual video bootstrap");
    for(const auto kind:{sb::Kind::Hello,sb::Kind::Ack,sb::Kind::Commit}){
        const auto prepared=sender.prepareDiscovery(bytes(ordinary),source());check(prepared.appended,"real prepared runtime signs actual video AU");
        std::vector<std::uint8_t> coded(prepared.output.data,prepared.output.data+prepared.output.size),restored;
        check(sb::removeOwned(bytes(coded),restored)==sb::SeiStatus::Ok&&restored==ordinary,"runtime never changes original VCL bytes");
        check(sender.complete(prepared.ticket,true,pa.channel)==sb::Status::Ok,"actual copied AU handoff receipt");
        const rx::AccessUnitContext context{authority,{31,6},true,true};const auto observed=receiver.observeAccessUnit(bytes(coded),context);
        check(observed.result==rx::Result::Handshake&&observed.original.data==coded.data()&&observed.original.size==coded.size(),"original signed AU enters real receiver exactly once");
        check(r.localConfig(rc,ra)&&r.peerVerified(),"actual video MAC now provides receiver binding");authority.channel=ra.channel;
        sb::Message reply;check(receiver.nextOutbound(reply)==sb::Status::Ok&&reply.kind==kind,"planned signed reverse reply");
        const auto body=wire(reply);std::uint64_t ticket=0;check(r.queueControl(bytes(body),ticket)&&ticket!=0,"actual reverse metadata queued for socket");
        sb::SendReceipt receipt{};check(!r.pollSent(receipt),"construction is not an actual socket send receipt");pump(p,r);
        check(r.pollSent(receipt)&&receipt.ticket==ticket,"all176 bytes accepted by real socket before send receipt");
        sb::VerifiedControl received;check(p.pollVerified(received)&&received.payload==body&&received.channel.binding==pa.channel.binding,"actual socket input MAC verified before consumer");
        check(sender.receiveControl(bytes(received.payload),received.channel)==sb::Status::Ok,"existing sender consumes verified socket reply");
        check(receiver.markOutboundSent(reply,authority)==sb::Status::Ok,"receiver marks exact successfully sent reverse action");
    }
    check(p.peerVerified()&&sender.mutualConfirmed()&&receiver.snapshot().peerConfirmed&&!sender.coveragePermitted(),"real loopback mutual auth still has no MR permission");
    std::uint64_t tickets[4]{};for(auto& ticket:tickets)check(r.queueControl(bytes(firstWire),ticket),"fixed control queue slot");
    std::uint64_t sentinel=91;check(!r.queueControl(bytes(firstWire),sentinel)&&sentinel==91,"fifth queued packet cannot grow memory");pump(p,r);
    for(auto ticket:tickets){sb::SendReceipt receipt;sb::VerifiedControl got;check(r.pollSent(receipt)&&receipt.ticket==ticket&&p.pollVerified(got)&&got.payload==firstWire,"bounded real transport preserves complete message order");}
    r.stop();pump(p,r);check(p.status()==sb::ControlStatus::Lost&&!p.peerVerified()&&!p.localConfig(pc,pa),"actual EOF immediately revokes current trust");
    const auto fallback=sender.prepareDiscovery(bytes(ordinary),source());check(!fallback.appended&&fallback.output.data==ordinary.data(),"lost runtime leaves ordinary video intact");
}
void partialAndInvalid(){
    Files files;sb::ControlRuntime runtime;auto req=request(files.producer,sb::Role::Producer);check(runtime.prepare(req)==sb::ControlStatus::Listening,"invalid-input listener");
    int raw=socket(AF_INET,SOCK_STREAM,0);check(raw>=0,"fixture raw socket");sockaddr_in endpoint{};endpoint.sin_family=AF_INET;endpoint.sin_port=htons(runtime.localPort());inet_pton(AF_INET,"127.0.0.1",&endpoint.sin_addr);
    check(connect(raw,reinterpret_cast<sockaddr*>(&endpoint),sizeof endpoint)==0,"fixture connects actual TCP");
    std::array<std::uint8_t,sb::AuthEnvelopeBytes> invalid{};
    check(send(raw,invalid.data(),40,0)==40,"fixture partial TCP write");for(unsigned i=0;i<30;++i)runtime.tick();
    check(runtime.status()==sb::ControlStatus::TransportConnected&&!runtime.peerVerified(),"partial TCP packet never becomes trusted metadata");
    check(send(raw,invalid.data()+40,invalid.size()-40,0)==static_cast<ssize_t>(invalid.size()-40),"fixture remaining TCP bytes");for(unsigned i=0;i<30;++i)runtime.tick();
    sb::VerifiedControl output;check(runtime.status()==sb::ControlStatus::AuthenticationRefused&&!runtime.peerVerified()&&!runtime.pollVerified(output),"complete unauthenticated packet closes trust without an output");close(raw);
}
}
int main(){try{credentials();defaultsAndAddress();capabilities();loopbackHandshake();partialAndInvalid();std::cout<<checks<<" private credential and actual loopback control checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
