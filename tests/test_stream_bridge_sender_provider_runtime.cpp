#include "pc/stream-bridge-sender-provider-runtime.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace rx=stream_bridge_receiver;
namespace {
unsigned checks=0;void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
void* encoder=reinterpret_cast<void*>(0x1010);void* output=reinterpret_cast<void*>(0x2020);std::vector<std::uint8_t> au{0,0,0,1,0x26,1,0x80,0x34,0x56};
NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** e){*e=encoder;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS reconfigureResult=NV_ENC_ERR_INVALID_PARAM;unsigned reconfigureCalls=0;
NV_ENC_RECONFIGURE_PARAMS* expectedReconfigure=nullptr;
NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS* input){++reconfigureCalls;check(!expectedReconfigure||input==expectedReconfigure,"native Reconfigure receives original caller parameter pointer");return reconfigureResult;}
NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM* p){p->bitstreamBufferPtr=au.data();p->bitstreamSizeInBytes=au.size();p->outputTimeStamp=7;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI reg(void*,NV_ENC_REGISTER_RESOURCE* p){p->registeredResource=reinterpret_cast<NV_ENC_REGISTERED_PTR>(0x70);return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unreg(void*,NV_ENC_REGISTERED_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI mapInput(void*,NV_ENC_MAP_INPUT_RESOURCE* p){p->mappedResource=reinterpret_cast<NV_ENC_INPUT_PTR>(0x80);p->mappedBufferFmt=NV_ENC_BUFFER_FORMAT_NV12;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unmapInput(void*,NV_ENC_INPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI encode(void*,NV_ENC_PIC_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI destroy(void*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* p){p->nvEncOpenEncodeSessionEx=open;p->nvEncInitializeEncoder=init;p->nvEncReconfigureEncoder=reconfigure;p->nvEncLockBitstream=lock;p->nvEncUnlockBitstream=unlock;p->nvEncDestroyEncoder=destroy;p->nvEncRegisterResource=reg;p->nvEncUnregisterResource=unreg;p->nvEncMapInputResource=mapInput;p->nvEncUnmapInputResource=unmapInput;p->nvEncEncodePicture=encode;return NV_ENC_SUCCESS;}
Nonce nonce(){Nonce n{};n[0]=91;return n;}
struct Files{
    std::filesystem::path path;
    Files(){char p[]="/tmp/sfnp-runtime712-XXXXXX";auto* made=mkdtemp(p);check(made!=nullptr,"fixture private directory");path=made;std::filesystem::create_directory(path/"producer");std::filesystem::create_directory(path/"receiver");
        const char* id="0102030405060708090a0b0c0d0e0f10";const char* secret="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
        for(const auto* role:{"producer","receiver"}){chmod((path/role).c_str(),0700);const auto active=path/role/"active.json",key=path/role/(std::string(id)+".json");std::ofstream(active)<<"{\"schema\":\"sfnp-active-pairing-v1\",\"pair_id\":\""<<id<<"\"}";std::ofstream(key)<<"{\"schema\":\"sfnp-pairing-v1\",\"pair_id\":\""<<id<<"\",\"key_hex\":\""<<secret<<"\",\"role\":\""<<role<<"\",\"protocol_major\":1}";chmod(active.c_str(),0600);chmod(key.c_str(),0600);}}
    ~Files(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
void run(){
    Files files;NvencBoundary boundary({{1}});SenderRuntime runtime(boundary);ControlRuntime reverse;
    ControlRequest request;request.role=Role::Producer;request.buildId=712;request.pairingsDirectory=files.path/"producer";std::strcpy(request.address.data(),"127.0.0.1");
    runtime.configure(request);runtime.tick();check(runtime.state()==RuntimeState::Off&&runtime.control().localPort()==0,"actual runtime defaults off without opening socket");
    request.explicitlyEnabled=true;runtime.configure(request);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder&&runtime.control().localPort()==0,"no real encoder cannot prepare discovery");
    NV_ENCODE_API_FUNCTION_LIST table{};table.version=NV_ENCODE_API_FUNCTION_LIST_VER;boundary.create(factory,&table);void* handle=nullptr;NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};op.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;table.nvEncOpenEncodeSessionEx(&op,&handle);
    NV_ENC_INITIALIZE_PARAMS ip{};ip.version=NV_ENC_INITIALIZE_PARAMS_VER;ip.encodeWidth=128;ip.encodeHeight=128;ip.encodeGUID=NV_ENC_CODEC_H264_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder,"actual H264 never starts HEVC metadata route");
    NV_ENC_CONFIG config{};config.version=NV_ENC_CONFIG_VER;config.rcParams.version=NV_ENC_CONFIG_VER;config.rcParams.averageBitRate=10000000;
    ip.frameRateNum=90;ip.frameRateDen=1;ip.encodeConfig=&config;
    ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder&&runtime.control().localPort()==0,"Init alone cannot open listener before an actual accepted encode");
    auto submit=[&](){NV_ENC_REGISTER_RESOURCE r{};r.version=NV_ENC_REGISTER_RESOURCE_VER;r.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;r.resourceToRegister=reinterpret_cast<void*>(0x60);r.width=r.height=128;r.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;r.bufferUsage=NV_ENC_INPUT_IMAGE;table.nvEncRegisterResource(handle,&r);NV_ENC_MAP_INPUT_RESOURCE m{};m.version=NV_ENC_MAP_INPUT_RESOURCE_VER;m.registeredResource=r.registeredResource;table.nvEncMapInputResource(handle,&m);NV_ENC_PIC_PARAMS p{};p.version=NV_ENC_PIC_PARAMS_VER;p.inputBuffer=m.mappedResource;p.outputBitstream=output;p.inputWidth=p.inputHeight=128;p.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;p.inputTimeStamp=7;table.nvEncEncodePicture(handle,&p);};
    submit();runtime.tick();check(runtime.state()==RuntimeState::WaitingPeer&&runtime.control().localPort()!=0,"actual HEVC Init triggers cold paired runtime/listener");
    const auto stableGeneration=boundary.generation(handle);const auto stablePort=runtime.control().localPort();
    unsigned qosCalls=0;
    auto qos=[&](){
        ++qosCalls;config.rcParams.averageBitRate=10000000+qosCalls*1000;config.rcParams.maxBitRate=12000000+qosCalls*1000;
        config.rcParams.vbvBufferSize=1000000+qosCalls;config.rcParams.vbvInitialDelay=500000+qosCalls;
        NV_ENC_RECONFIGURE_PARAMS value{};value.version=NV_ENC_RECONFIGURE_PARAMS_VER;value.reInitEncodeParams=ip;
        value.reInitEncodeParams.frameRateNum=qosCalls%2?90:120;
        const auto before=value;const auto beforeConfig=config;
        reconfigureResult=qosCalls%2?NV_ENC_SUCCESS:NV_ENC_ERR_INVALID_PARAM;expectedReconfigure=&value;
        check(table.nvEncReconfigureEncoder(handle,&value)==reconfigureResult,"native QoS success/rejection returned unchanged");expectedReconfigure=nullptr;
        check(!std::memcmp(&value,&before,sizeof value)&&!std::memcmp(&config,&beforeConfig,sizeof config),"caller parameters and pointed configuration never rewritten");
        check(boundary.generation(handle)==stableGeneration&&boundary.encoderStates()[0].encoded,"ordinary bitrate adaptation keeps the live encoded session");
        runtime.tick();check(runtime.control().localPort()==stablePort,"healthy ordinary QoS keeps actual TCP listener and paired channel");
    };
    qos();
    ControlRequest rr=request;rr.role=Role::Receiver;rr.pairingsDirectory=files.path/"receiver";rr.port=runtime.control().localPort();rr.video={nonce(),4};
    check(reverse.prepare(rr)==ControlStatus::Connecting||reverse.status()==ControlStatus::WaitingVideo,"real receiver reverse socket started");
    auto pump=[&](){for(unsigned i=0;i<80;++i){if(i%7==0)qos();reverse.tick();runtime.tick();}};pump();
    LocalConfig receiverLocal;reverse.configuredLocal(receiverLocal);rx::Receiver receiver;rx::Authority authority{rr.video.connection,rr.video.generation,{}};
    check(receiver.beginAuthenticated(authority,receiverLocal,reverse.receiverVerifier(),reverse.verifierContext()),"actual runtime connects existing authenticated receiver port");
    std::uint16_t nativeKey=1;
    auto receiveVideo=[&](){NV_ENC_LOCK_BITSTREAM p{};p.version=NV_ENC_LOCK_BITSTREAM_VER;p.outputBitstream=output;check(table.nvEncLockBitstream(handle,&p)==NV_ENC_SUCCESS&&p.bitstreamBufferPtr!=au.data(),"actual NVENC Lock emits ordered protected discovery/probe");std::vector<std::uint8_t> copied(static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),static_cast<const std::uint8_t*>(p.bitstreamBufferPtr)+p.bitstreamSizeInBytes);table.nvEncUnlockBitstream(handle,output);std::vector<std::uint8_t> original;check(removeOwned({copied.data(),copied.size()},original)==SeiStatus::Ok&&original==au,"all original VCL bytes preserved through actual Lock/Unlock");const rx::AccessUnitContext proof{authority,{nativeKey++,8},true,true};const auto result=receiver.observeAccessUnit({copied.data(),copied.size()},proof);LocalSendAuthority binding;LocalConfig config;check(reverse.localConfig(config,binding),"actual video authorizes reverse channel after MAC verification");authority.channel=binding.channel;return result.result;};
    for(const auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){
        check(receiveVideo()==rx::Result::Handshake,"actual native output discovery reaches receiver");Message reply;check(receiver.nextOutbound(reply)==Status::Ok&&reply.kind==kind,"receiver schedules exact handshake response");AuthPayload body{};encode(reply,body);std::uint64_t ticket=0;check(reverse.queueControl({body.data(),body.size()},ticket),"reverse action enters actual bounded TCP");pump();SendReceipt sent;check(reverse.pollSent(sent)&&sent.ticket==ticket,"actual socket handoff receipt");check(receiver.markOutboundSent(reply,authority)==Status::Ok,"receiver advances only after socket send");}
    pump();check(runtime.state()==RuntimeState::Confirmed&&receiver.snapshot().peerConfirmed,"actual provider worker mutually confirms existing native output session");
    check(runtime.lastFailure().reason==RuntimeFailure::None,"healthy actual handshake has no failure classification");
    check(receiveVideo()==rx::Result::MetadataObserved,"real native AU carries fresh post-handshake probe");ProbeMessage echo;check(receiver.nextEcho(echo),"probe echoed only after actual coded-AU observation");AuthPayload encoded{};encodeProbe(echo,encoded);std::uint64_t ticket=0;check(reverse.queueControl({encoded.data(),encoded.size()},ticket),"exact observed probe echo enters reverse transport");pump();check(runtime.state()==RuntimeState::ProbeObserved,"actual provider worker verifies matching roundtrip probe");
    const auto stats=boundary.reconfigureStats();check(stats.total==qosCalls&&stats.preservedSuccessful==(qosCalls+1)/2&&stats.preservedRejected==qosCalls/2&&stats.revoked==0&&reconfigureCalls==qosCalls,"bounded cold counters classify every repeated accepted/rejected QoS call");
    check(!receiver.snapshot().metadataSelected,"probe transport does not fabricate selected GPU identity");
    reverse.stop();for(unsigned i=0;i<80;++i){reverse.tick();runtime.tick();}check(runtime.state()==RuntimeState::Failed&&!runtime.control().peerVerified(),"actual reverse EOF revokes provider handshake without a timeout");
    const auto lost=runtime.lastFailure();check(lost.reason==RuntimeFailure::TransportLost&&lost.control==ControlStatus::Lost
        &&!lost.hasProtocolStatus&&lost.encoder.generation==stableGeneration&&lost.encoder.hevc&&lost.encoder.encoded,
        "real reverse EOF preserves exact pre-revocation transport and encoder facts");
    NV_ENC_LOCK_BITSTREAM fallback{};fallback.version=NV_ENC_LOCK_BITSTREAM_VER;fallback.outputBitstream=output;table.nvEncLockBitstream(handle,&fallback);check(fallback.bitstreamBufferPtr==au.data()&&fallback.bitstreamSizeInBytes==au.size(),"lost paired peer leaves ordinary actual native AU pointer and size intact");table.nvEncUnlockBitstream(handle,output);
    const auto previous=runtime.encoder().generation;NV_ENC_RECONFIGURE_PARAMS rc{};rc.version=NV_ENC_RECONFIGURE_PARAMS_VER;reconfigureResult=NV_ENC_ERR_INVALID_PARAM;table.nvEncReconfigureEncoder(handle,&rc);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder&&!runtime.control().peerVerified()&&runtime.control().localPort()==0,"real source reconfiguration tears down authenticated transport immediately");
    ip.encodeGUID=NV_ENC_CODEC_H264_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder,"non-HEVC source remains ordinary after reconfiguration");
    ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;table.nvEncInitializeEncoder(handle,&ip);submit();runtime.tick();check(runtime.encoder().generation>previous&&runtime.state()==RuntimeState::WaitingPeer,"new actual codec epoch gets fresh session and must reconfirm");
    check(runtime.lastFailure().reason==RuntimeFailure::TransportLost&&runtime.lastFailure().encoder.generation==stableGeneration,
        "later source admission retains the earlier failure snapshot for bounded cold reporting");
    auto missing=request;missing.pairingsDirectory=files.path/"missing";runtime.configure(missing);runtime.tick();
    check(runtime.state()==RuntimeState::Failed&&runtime.lastFailure().reason==RuntimeFailure::PrepareControl
        &&runtime.lastFailure().control==ControlStatus::PairingRefused&&runtime.control().status()==ControlStatus::Disabled,
        "actual missing credential reports prepare refusal before cleanup replaces the native status");
    runtime.configure(request);runtime.tick();runtime.control().stop();runtime.tick();
    check(runtime.lastFailure().reason==RuntimeFailure::TransportDisabled&&runtime.lastFailure().control==ControlStatus::Disabled,
        "actual local transport stop is classified separately from peer loss");
    runtime.configure(request);runtime.tick();
    NV_ENC_LOCK_BITSTREAM leased{};leased.version=NV_ENC_LOCK_BITSTREAM_VER;leased.outputBitstream=output;
    check(table.nvEncLockBitstream(handle,&leased)==NV_ENC_SUCCESS&&leased.bitstreamBufferPtr!=au.data()&&boundary.liveLeases()==1,
        "actual augmented output stays borrowed while a new cold admission is attempted");
    runtime.configure(request);runtime.tick();const auto denied=runtime.lastFailure();
    check(runtime.state()==RuntimeState::Failed&&denied.reason==RuntimeFailure::RequestDiscovery
        &&denied.control==ControlStatus::Listening&&denied.liveOutputLeases==1&&denied.encoder.generation==boundary.generation(handle),
        "actual leased output refusal reports discovery admission and outstanding native lease count");
    table.nvEncUnlockBitstream(handle,output);
    runtime.configure(request);runtime.tick();
    const int raw=socket(AF_INET,SOCK_STREAM,0);check(raw>=0,"actual invalid-control peer socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(runtime.control().localPort());inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    check(connect(raw,reinterpret_cast<const sockaddr*>(&address),sizeof address)==0,"actual unauthenticated peer reaches control endpoint");
    const AuthEnvelope invalid{};check(send(raw,invalid.data(),invalid.size(),0)==static_cast<ssize_t>(invalid.size()),"actual invalid control envelope sent");
    for(unsigned i=0;i<80;++i)runtime.tick();
    close(raw);
    check(runtime.lastFailure().reason==RuntimeFailure::TransportAuthenticationRefused&&runtime.lastFailure().control==ControlStatus::AuthenticationRefused,
        "actual MAC failure is classified separately from EOF and disabled transport");
    runtime.configure(request);runtime.tick();rr.port=runtime.control().localPort();++rr.video.connection[0];++rr.video.generation;reverse.prepare(rr);
    for(unsigned i=0;i<80;++i){reverse.tick();runtime.tick();}
    reverse.configuredLocal(receiverLocal);authority={rr.video.connection,rr.video.generation,{}};
    check(receiver.beginAuthenticated(authority,receiverLocal,reverse.receiverVerifier(),reverse.verifierContext())&&receiveVideo()==rx::Result::Handshake,
        "new real signed video bootstrap enables authenticated reverse-protocol refusal fixture");
    Message outOfOrder;AuthPayload malformed{};std::uint64_t invalidTicket=0;
    check(receiver.nextOutbound(outOfOrder)==Status::Ok,"actual receiver schedules its initial handshake reply");
    outOfOrder.kind=Kind::Ack;outOfOrder.peerNonce.fill(44);outOfOrder.selected=outOfOrder.offered;
    check(encode(outOfOrder,malformed)==Status::Ok&&reverse.queueControl({malformed.data(),malformed.size()},invalidTicket),
        "real authenticated channel seals a valid but out-of-order protocol action");
    for(unsigned i=0;i<80;++i){reverse.tick();runtime.tick();}
    check(runtime.lastFailure().reason==RuntimeFailure::ReceiveControl&&runtime.lastFailure().hasProtocolStatus
        &&runtime.lastFailure().protocol!=Status::Ok&&runtime.lastFailure().control==ControlStatus::TransportConnected,
        "actual authenticated invalid protocol records the exact receive-control status before socket cleanup");
    reverse.stop();
    runtime.stop();runtime.tick();check(runtime.state()==RuntimeState::Off&&runtime.control().localPort()==0,"explicit stop never autonomously restarts control");table.nvEncDestroyEncoder(handle);boundary.stop();
    check(runtime.lastFailure().reason==RuntimeFailure::None,"explicit stop clears prior session diagnostic state");
}
}
int main(){try{run();std::printf("sender actual provider/control composition: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"sender runtime failed after %u: %s\n",checks,e.what());return 1;}}
