#include "pc/stream-bridge-sender-provider-runtime.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace stream_bridge;using namespace stream_bridge::sender;
namespace rx=stream_bridge_receiver;
namespace {
unsigned checks=0;void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
void* encoder=reinterpret_cast<void*>(0x1010);void* output=reinterpret_cast<void*>(0x2020);std::vector<std::uint8_t> au{0,0,0,1,0x26,1,0x80,0x34,0x56};
NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void** e){*e=encoder;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*){return NV_ENC_ERR_INVALID_PARAM;}
NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM* p){p->bitstreamBufferPtr=au.data();p->bitstreamSizeInBytes=au.size();p->outputTimeStamp=7;return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI destroy(void*){return NV_ENC_SUCCESS;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* p){p->nvEncOpenEncodeSessionEx=open;p->nvEncInitializeEncoder=init;p->nvEncReconfigureEncoder=reconfigure;p->nvEncLockBitstream=lock;p->nvEncUnlockBitstream=unlock;p->nvEncDestroyEncoder=destroy;return NV_ENC_SUCCESS;}
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
    NV_ENC_INITIALIZE_PARAMS ip{};ip.version=NV_ENC_INITIALIZE_PARAMS_VER;ip.encodeGUID=NV_ENC_CODEC_H264_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder,"actual H264 never starts HEVC metadata route");
    ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.state()==RuntimeState::WaitingPeer&&runtime.control().localPort()!=0,"actual HEVC Init triggers cold paired runtime/listener");
    ControlRequest rr=request;rr.role=Role::Receiver;rr.pairingsDirectory=files.path/"receiver";rr.port=runtime.control().localPort();rr.video={nonce(),4};
    check(reverse.prepare(rr)==ControlStatus::Connecting||reverse.status()==ControlStatus::WaitingVideo,"real receiver reverse socket started");
    auto pump=[&](){for(unsigned i=0;i<80;++i){reverse.tick();runtime.tick();}};pump();
    LocalConfig receiverLocal;reverse.configuredLocal(receiverLocal);rx::Receiver receiver;rx::Authority authority{rr.video.connection,rr.video.generation,{}};
    check(receiver.beginAuthenticated(authority,receiverLocal,reverse.receiverVerifier(),reverse.verifierContext()),"actual runtime connects existing authenticated receiver port");
    std::uint16_t nativeKey=1;
    auto receiveVideo=[&](){NV_ENC_LOCK_BITSTREAM p{};p.version=NV_ENC_LOCK_BITSTREAM_VER;p.outputBitstream=output;check(table.nvEncLockBitstream(handle,&p)==NV_ENC_SUCCESS&&p.bitstreamBufferPtr!=au.data(),"actual NVENC Lock emits ordered protected discovery/probe");std::vector<std::uint8_t> copied(static_cast<const std::uint8_t*>(p.bitstreamBufferPtr),static_cast<const std::uint8_t*>(p.bitstreamBufferPtr)+p.bitstreamSizeInBytes);table.nvEncUnlockBitstream(handle,output);std::vector<std::uint8_t> original;check(removeOwned({copied.data(),copied.size()},original)==SeiStatus::Ok&&original==au,"all original VCL bytes preserved through actual Lock/Unlock");const rx::AccessUnitContext proof{authority,{nativeKey++,8},true,true};const auto result=receiver.observeAccessUnit({copied.data(),copied.size()},proof);LocalSendAuthority binding;LocalConfig config;check(reverse.localConfig(config,binding),"actual video authorizes reverse channel after MAC verification");authority.channel=binding.channel;return result.result;};
    for(const auto kind:{Kind::Hello,Kind::Ack,Kind::Commit}){
        check(receiveVideo()==rx::Result::Handshake,"actual native output discovery reaches receiver");Message reply;check(receiver.nextOutbound(reply)==Status::Ok&&reply.kind==kind,"receiver schedules exact handshake response");AuthPayload body{};encode(reply,body);std::uint64_t ticket=0;check(reverse.queueControl({body.data(),body.size()},ticket),"reverse action enters actual bounded TCP");pump();SendReceipt sent;check(reverse.pollSent(sent)&&sent.ticket==ticket,"actual socket handoff receipt");check(receiver.markOutboundSent(reply,authority)==Status::Ok,"receiver advances only after socket send");}
    pump();check(runtime.state()==RuntimeState::Confirmed&&receiver.snapshot().peerConfirmed,"actual provider worker mutually confirms existing native output session");
    check(receiveVideo()==rx::Result::MetadataObserved,"real native AU carries fresh post-handshake probe");ProbeMessage echo;check(receiver.nextEcho(echo),"probe echoed only after actual coded-AU observation");AuthPayload encoded{};encodeProbe(echo,encoded);std::uint64_t ticket=0;check(reverse.queueControl({encoded.data(),encoded.size()},ticket),"exact observed probe echo enters reverse transport");pump();check(runtime.state()==RuntimeState::ProbeObserved,"actual provider worker verifies matching roundtrip probe");
    check(!receiver.snapshot().metadataSelected,"probe transport does not fabricate selected GPU identity");
    reverse.stop();pump();check(runtime.state()==RuntimeState::Failed&&!runtime.control().peerVerified(),"actual reverse EOF revokes provider handshake without a timeout");
    NV_ENC_LOCK_BITSTREAM fallback{};fallback.version=NV_ENC_LOCK_BITSTREAM_VER;fallback.outputBitstream=output;table.nvEncLockBitstream(handle,&fallback);check(fallback.bitstreamBufferPtr==au.data()&&fallback.bitstreamSizeInBytes==au.size(),"lost paired peer leaves ordinary actual native AU pointer and size intact");table.nvEncUnlockBitstream(handle,output);
    const auto previous=runtime.encoder().generation;NV_ENC_RECONFIGURE_PARAMS rc{};rc.version=NV_ENC_RECONFIGURE_PARAMS_VER;table.nvEncReconfigureEncoder(handle,&rc);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder&&!runtime.control().peerVerified()&&runtime.control().localPort()==0,"real source reconfiguration tears down authenticated transport immediately");
    ip.encodeGUID=NV_ENC_CODEC_H264_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.state()==RuntimeState::WaitingEncoder,"non-HEVC source remains ordinary after reconfiguration");
    ip.encodeGUID=NV_ENC_CODEC_HEVC_GUID;table.nvEncInitializeEncoder(handle,&ip);runtime.tick();check(runtime.encoder().generation>previous&&runtime.state()==RuntimeState::WaitingPeer,"new actual codec epoch gets fresh session and must reconfirm");
    runtime.stop();runtime.tick();check(runtime.state()==RuntimeState::Off&&runtime.control().localPort()==0,"explicit stop never autonomously restarts control");table.nvEncDestroyEncoder(handle);boundary.stop();
}
}
int main(){try{run();std::printf("sender actual provider/control composition: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"sender runtime failed after %u: %s\n",checks,e.what());return 1;}}
