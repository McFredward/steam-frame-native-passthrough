#include "stream_bridge_control.h"
#include "stream_bridge_capacity.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace stream_bridge {
namespace {
#ifdef _WIN32
using Socket=SOCKET;constexpr Socket Invalid=INVALID_SOCKET;
int socketError(){return WSAGetLastError();}
bool pending(int error){return error==WSAEWOULDBLOCK||error==WSAEINPROGRESS||error==WSAEALREADY||error==WSAENOTCONN;}
void closeSocket(Socket fd){if(fd!=Invalid)closesocket(fd);}
bool nonblocking(Socket fd){u_long value=1;return ioctlsocket(fd,FIONBIO,&value)==0;}
#else
using Socket=int;constexpr Socket Invalid=-1;
int socketError(){return errno;}
bool pending(int error){return error==EAGAIN||error==EWOULDBLOCK||error==EINPROGRESS||error==EALREADY||error==ENOTCONN||error==EINTR;}
void closeSocket(Socket fd){if(fd!=Invalid)::close(fd);}
bool nonblocking(Socket fd){const int flags=fcntl(fd,F_GETFL,0);return flags>=0&&fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0&&fcntl(fd,F_SETFD,FD_CLOEXEC)==0;}
#endif
bool nonzero(const Nonce& n){return std::any_of(n.begin(),n.end(),[](auto b){return b!=0;});}
bool configureSocket(Socket fd){const int enabled=1;return nonblocking(fd)
    &&setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&enabled),sizeof enabled)==0;}
bool address(const ControlRequest& request,sockaddr_in& out){
    if(std::find(request.address.begin(),request.address.end(),0)==request.address.end())return false;
    out={};out.sin_family=AF_INET;out.sin_port=htons(request.port);
    if(inet_pton(AF_INET,request.address.data(),&out.sin_addr)!=1)return false;
    const auto host=ntohl(out.sin_addr.s_addr);
    return (host>>24)==127||(host>>24)==10||(host>>20)==0xac1||(host>>16)==0xc0a8;
}
}
struct ControlRuntime::Impl{
    static constexpr std::size_t Capacity=4;
    struct Outbound{AuthEnvelope bytes{};std::size_t sent=0;std::uint64_t ticket=0;};
    mutable std::mutex mutex;AuthSession auth;LocalConfig config{};VideoStreamContext video{};
    CapacityProtection capacityProtection;Nonce budgetBinding{},challenge{};
    std::uint32_t nativeBudget=0,peerBudget=0;bool requireBudget=false,budgetChallengeSent=false,budgetAnswered=false;
    ControlStatus state=ControlStatus::Disabled;CredentialStatus credential=CredentialStatus::MissingSelection;
    Socket listener=Invalid,peer=Invalid;std::uint16_t port=0;bool prepared=false,connecting=false;
#ifdef _WIN32
    bool winsock=false;
#endif
    std::array<Outbound,Capacity> tx{};std::size_t txHead=0,txCount=0;
    std::array<VerifiedControl,Capacity> verified{};std::size_t rxHead=0,rxCount=0;
    std::array<SendReceipt,Capacity> sent{};std::size_t sentHead=0,sentCount=0;
    AuthEnvelope assembling{};std::size_t assembled=0;std::uint64_t nextTicket=0;
    void close(ControlStatus why){
        closeSocket(peer);closeSocket(listener);peer=listener=Invalid;port=0;connecting=prepared=false;
        auth.reset();capacityProtection.reset();config={};video={};
        budgetBinding={};challenge={};nativeBudget=peerBudget=0;requireBudget=budgetChallengeSent=budgetAnswered=false;tx={};verified={};sent={};assembling={};
        txHead=txCount=rxHead=rxCount=sentHead=sentCount=assembled=0;state=why;
#ifdef _WIN32
        if(winsock){WSACleanup();winsock=false;}
#endif
    }
};
ControlRuntime::ControlRuntime():impl_(new Impl){}
ControlRuntime::~ControlRuntime(){stop();}
void ControlRuntime::stop() noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->close(ControlStatus::Disabled);}
ControlStatus ControlRuntime::prepare(const ControlRequest& request) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->close(ControlStatus::Disabled);
    if(!request.explicitlyEnabled)return impl_->state;
    if((request.role!=Role::Producer&&request.role!=Role::Receiver)||!request.buildId
        ||(request.offered&~KnownCapabilities)||(request.required&~request.offered)
        ||(request.required&RequiredCapabilities)!=RequiredCapabilities
        ||(request.role==Role::Receiver&&(!nonzero(request.video.connection)||!request.video.generation)))return impl_->state=ControlStatus::AddressRefused;
    const bool full=(request.required&CapExplicitCoverage)!=0;
    if(full&&request.role==Role::Receiver&&(request.nativeAccessUnitCapacity<512||request.nativeAccessUnitCapacity>MaximumAccessUnitBytes))return impl_->state=ControlStatus::CapacityRefused;
    sockaddr_in endpoint{};if(!address(request,endpoint)||(request.role==Role::Receiver&&(!request.port||endpoint.sin_addr.s_addr==INADDR_ANY)))return impl_->state=ControlStatus::AddressRefused;
    Pairing pair;struct Erase{Pairing& p;~Erase(){secureZero(p.key.data(),p.key.size());}}erase{pair};
    impl_->credential=loadPrivatePairing(request.pairingsDirectory,request.role,pair);
    if(impl_->credential!=CredentialStatus::Ok)return impl_->state=ControlStatus::PairingRefused;
    Nonce local{},binding{};
    if(!bridgeSystemRandom(local.data(),local.size())||!nonzero(local)
        ||(request.role==Role::Producer&&(!bridgeSystemRandom(binding.data(),binding.size())||!nonzero(binding))))return impl_->state=ControlStatus::RandomRefused;
    const auto auth=request.role==Role::Producer?impl_->auth.prepareProducer(pair,binding):impl_->auth.prepareReceiver(pair,request.video);
    if(auth!=AuthStatus::Ok||!impl_->capacityProtection.prepare(pair))return impl_->state=ControlStatus::AuthenticationRefused;
    impl_->requireBudget=full;impl_->nativeBudget=request.nativeAccessUnitCapacity;impl_->challenge=local;
    impl_->config={request.role,request.buildId,local,request.offered,request.required};impl_->video=request.video;
#ifdef _WIN32
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0){impl_->close(ControlStatus::IoRefused);return impl_->state;}impl_->winsock=true;
#endif
    Socket socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(socket==Invalid||!configureSocket(socket)){closeSocket(socket);impl_->close(ControlStatus::IoRefused);return impl_->state;}
    if(request.role==Role::Producer){
        if(bind(socket,reinterpret_cast<const sockaddr*>(&endpoint),sizeof endpoint)!=0||listen(socket,1)!=0){closeSocket(socket);impl_->close(ControlStatus::IoRefused);return impl_->state;}
        impl_->listener=socket;
#ifdef _WIN32
        int size=sizeof endpoint;
#else
        socklen_t size=sizeof endpoint;
#endif
        if(getsockname(socket,reinterpret_cast<sockaddr*>(&endpoint),&size)!=0){impl_->close(ControlStatus::IoRefused);return impl_->state;}
        impl_->port=ntohs(endpoint.sin_port);impl_->state=ControlStatus::Listening;
    }else{
        impl_->peer=socket;const int result=connect(socket,reinterpret_cast<const sockaddr*>(&endpoint),sizeof endpoint);
        if(result!=0&&!pending(socketError())){impl_->close(ControlStatus::IoRefused);return impl_->state;}
        impl_->connecting=result!=0;impl_->state=impl_->connecting?ControlStatus::Connecting:ControlStatus::WaitingVideo;
    }
    impl_->prepared=true;return impl_->state;
}
ControlStatus ControlRuntime::status()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->state;}
CredentialStatus ControlRuntime::credentialStatus()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->credential;}
std::uint16_t ControlRuntime::localPort()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->port;}
bool ControlRuntime::peerVerified()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->auth.peerVerified();}
std::uint32_t ControlRuntime::peerAccessUnitCapacity()const noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->peerBudget;}
bool ControlRuntime::certifyAccessUnit(Bytes source,const FrameDescriptor& descriptor,CoverageEncoding encoding,
    Bytes coverage,std::vector<std::uint8_t>& output) noexcept{
    try{
        std::unique_lock<std::mutex> lock(impl_->mutex,std::try_to_lock);
        if(!lock.owns_lock()||!impl_->prepared||impl_->config.role!=Role::Producer)return false;
        if(impl_->requireBudget&&!impl_->peerBudget)return false;
        std::vector<std::uint8_t> candidate;
        if(appendCertifiedFrame(source,descriptor,encoding,coverage,impl_->auth,candidate)!=FrameStatus::Ok||
            (impl_->requireBudget&&candidate.size()>impl_->peerBudget))return false;
        output.swap(candidate);return true;
    }catch(...){return false;}
}
bool ControlRuntime::verifyAccessUnit(Bytes source,const stream_bridge_receiver::AccessUnitContext& context,
    CertifiedAccessUnit& output) noexcept{
    try{
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(!impl_->prepared||impl_->config.role!=Role::Receiver||!context.sourceBoundaryVerified||!context.native.generation||
            context.authority.session!=impl_->video.connection||context.authority.streamGeneration!=impl_->video.generation)return false;
        CertifiedAccessUnit candidate;
        if(inspectCertifiedFrame(source,impl_->auth,candidate)!=FrameStatus::Ok)return false;
        if(!impl_->auth.bound()){
            std::vector<std::uint8_t> hello;
            if(inspectOwned({candidate.original.data(),candidate.original.size()},hello)!=SeiStatus::Ok||
                impl_->auth.verifyInitialVideoHello({hello.data(),hello.size()},candidate.binding)!=AuthStatus::Ok)return false;
        }
        output=std::move(candidate);return true;
    }catch(...){return false;}
}
bool ControlRuntime::configuredLocal(LocalConfig& output)const noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->prepared)return false;output=impl_->config;return true;
}
bool ControlRuntime::localConfig(LocalConfig& config,LocalSendAuthority& authority)const noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);LocalSendAuthority local;
    if(!impl_->prepared||impl_->auth.localSendAuthority(local)!=AuthStatus::Ok)return false;
    config=impl_->config;authority=local;return true;
}
bool ControlRuntime::sealVideo(Bytes payload,std::vector<std::uint8_t>& output,void* context){
    auto& self=*static_cast<ControlRuntime*>(context);std::unique_lock<std::mutex> lock(self.impl_->mutex,std::try_to_lock);
    if(!lock||!self.impl_->prepared||self.impl_->config.role!=Role::Producer||
        (self.impl_->requireBudget&&!self.impl_->peerBudget))return false;
    AuthEnvelope envelope{};if(self.impl_->auth.seal(payload,envelope)!=AuthStatus::Ok)return false;
    output.assign(envelope.begin(),envelope.end());return true;
}
sender::PayloadSealer ControlRuntime::senderSealer() noexcept{return {sealVideo,this};}
stream_bridge_receiver::PayloadVerifier ControlRuntime::receiverVerifier() noexcept{return verifyVideo;}
void* ControlRuntime::verifierContext() noexcept{return this;}
bool ControlRuntime::verifyVideo(Bytes envelope,const stream_bridge_receiver::AccessUnitContext& context,
    AuthPayload& plain,VerifiedChannel& channel,void* owner) noexcept{
    auto& self=*static_cast<ControlRuntime*>(owner);std::lock_guard<std::mutex> lock(self.impl_->mutex);
    if(!self.impl_->prepared||self.impl_->config.role!=Role::Receiver||!context.sourceBoundaryVerified
        ||!context.completeAnnexBAccessUnit||!context.native.generation
        ||context.authority.session!=self.impl_->video.connection
        ||context.authority.streamGeneration!=self.impl_->video.generation)return false;
    if(self.impl_->requireBudget&&(!self.impl_->budgetAnswered||!envelope.data||envelope.size!=AuthEnvelopeBytes||!std::equal(self.impl_->budgetBinding.begin(),self.impl_->budgetBinding.end(),envelope.data+32)))return false;
    if(!self.impl_->auth.bound()){
        const VideoSourceEvidence evidence{{context.authority.session,context.authority.streamGeneration},
            context.sourceBoundaryVerified&&context.completeAnnexBAccessUnit&&context.native.generation!=0};
        if(self.impl_->auth.bootstrapFromVideo(envelope,evidence,plain,channel)!=AuthStatus::Ok)return false;
    }else if(self.impl_->auth.openControl(envelope,plain,channel)!=AuthStatus::Ok)return false;
    if(self.impl_->peer!=Invalid&&!self.impl_->connecting)self.impl_->state=ControlStatus::TransportConnected;
    return true;
}
bool ControlRuntime::queueControl(Bytes plain,std::uint64_t& ticket) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!impl_->prepared||impl_->config.role!=Role::Receiver||!impl_->auth.peerVerified()
        ||impl_->txCount==Impl::Capacity||impl_->nextTicket==~std::uint64_t(0))return false;
    AuthEnvelope envelope{};if(impl_->auth.seal(plain,envelope)!=AuthStatus::Ok)return false;
    auto& entry=impl_->tx[(impl_->txHead+impl_->txCount)%Impl::Capacity];entry={envelope,0,++impl_->nextTicket};
    ++impl_->txCount;ticket=entry.ticket;return true;
}
bool ControlRuntime::pollSent(SendReceipt& output) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->sentCount)return false;
    output=impl_->sent[impl_->sentHead];impl_->sentHead=(impl_->sentHead+1)%Impl::Capacity;--impl_->sentCount;return true;
}
bool ControlRuntime::pollVerified(VerifiedControl& output) noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->rxCount)return false;
    output=impl_->verified[impl_->rxHead];impl_->rxHead=(impl_->rxHead+1)%Impl::Capacity;--impl_->rxCount;return true;
}
void ControlRuntime::tick() noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->prepared)return;
    if(impl_->listener!=Invalid&&impl_->peer==Invalid){
        Socket peer=accept(impl_->listener,nullptr,nullptr);
        if(peer!=Invalid){if(!configureSocket(peer)){closeSocket(peer);return;}impl_->peer=peer;impl_->state=ControlStatus::TransportConnected;}
        else if(!pending(socketError())){impl_->close(ControlStatus::Lost);return;}
    }
    if(impl_->peer==Invalid)return;
    if(impl_->connecting){
        int error=0;
#ifdef _WIN32
        int count=sizeof error;
#else
        socklen_t count=sizeof error;
#endif
        if(getsockopt(impl_->peer,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&count)!=0||error){impl_->close(ControlStatus::Lost);return;}
        sockaddr_in address{};
#ifdef _WIN32
        int size=sizeof address;
#else
        socklen_t size=sizeof address;
#endif
        if(getpeername(impl_->peer,reinterpret_cast<sockaddr*>(&address),&size)!=0){if(!pending(socketError()))impl_->close(ControlStatus::Lost);return;}
        impl_->connecting=false;impl_->state=impl_->auth.bound()?ControlStatus::TransportConnected:ControlStatus::WaitingVideo;
    }
    if(impl_->requireBudget&&impl_->config.role==Role::Producer&&!impl_->budgetChallengeSent){
        if(impl_->txCount==Impl::Capacity)return;
        CapacityMessage message;message.binding=impl_->auth.binding();message.challenge=impl_->challenge;
        AuthEnvelope bytes{};if(!impl_->capacityProtection.seal(message,bytes)){impl_->close(ControlStatus::AuthenticationRefused);return;}
        impl_->tx[(impl_->txHead+impl_->txCount)%Impl::Capacity]={bytes,0,0};++impl_->txCount;impl_->budgetChallengeSent=true;
    }
    for(unsigned attempt=0;attempt<8&&impl_->txCount&&impl_->sentCount<Impl::Capacity;++attempt){
        auto& entry=impl_->tx[impl_->txHead];const auto needed=AuthEnvelopeBytes-entry.sent;
#ifdef _WIN32
        const auto n=send(impl_->peer,reinterpret_cast<const char*>(entry.bytes.data()+entry.sent),static_cast<int>(needed),0);
#else
        const auto n=send(impl_->peer,entry.bytes.data()+entry.sent,needed,MSG_NOSIGNAL);
#endif
        if(n<0){if(pending(socketError()))break;impl_->close(ControlStatus::Lost);return;}
        if(n==0){impl_->close(ControlStatus::Lost);return;}
        entry.sent+=static_cast<std::size_t>(n);
        if(entry.sent==AuthEnvelopeBytes){if(entry.ticket){impl_->sent[(impl_->sentHead+impl_->sentCount)%Impl::Capacity]={entry.ticket};++impl_->sentCount;}impl_->txHead=(impl_->txHead+1)%Impl::Capacity;--impl_->txCount;}
    }
    // Producer receives only reverse control. Receiver treats any application
    // payload on this direction as a protocol/transport failure, never video.
    for(unsigned attempt=0;attempt<8&&impl_->rxCount<Impl::Capacity;++attempt){
#ifdef _WIN32
        const auto n=recv(impl_->peer,reinterpret_cast<char*>(impl_->assembling.data()+impl_->assembled),static_cast<int>(AuthEnvelopeBytes-impl_->assembled),0);
#else
        const auto n=recv(impl_->peer,impl_->assembling.data()+impl_->assembled,AuthEnvelopeBytes-impl_->assembled,0);
#endif
        if(n<0){if(pending(socketError()))break;impl_->close(ControlStatus::Lost);return;}
        if(n==0){impl_->close(ControlStatus::Lost);return;}
        impl_->assembled+=static_cast<std::size_t>(n);
        if(impl_->assembled!=AuthEnvelopeBytes)continue;
        if(impl_->requireBudget&&!std::memcmp(impl_->assembling.data(),"SFC1",4)){
            CapacityMessage message;
            if(!impl_->capacityProtection.open({impl_->assembling.data(),impl_->assembling.size()},message)){impl_->close(ControlStatus::AuthenticationRefused);return;}
            if(impl_->config.role==Role::Producer){
                if(!impl_->budgetChallengeSent||impl_->peerBudget||message.binding!=impl_->auth.binding()||message.challenge!=impl_->challenge){impl_->close(ControlStatus::AuthenticationRefused);return;}
                impl_->peerBudget=message.maximumAccessUnitBytes;
            }else{
                if(impl_->budgetAnswered||impl_->txCount==Impl::Capacity){impl_->close(ControlStatus::AuthenticationRefused);return;}
                impl_->budgetBinding=message.binding;message.role=Role::Receiver;message.source=impl_->video;message.maximumAccessUnitBytes=impl_->nativeBudget;
                AuthEnvelope bytes{};if(!impl_->capacityProtection.seal(message,bytes)){impl_->close(ControlStatus::AuthenticationRefused);return;}
                impl_->tx[(impl_->txHead+impl_->txCount)%Impl::Capacity]={bytes,0,0};++impl_->txCount;impl_->budgetAnswered=true;
            }
            impl_->assembled=0;continue;
        }
        VerifiedControl incoming;
        if(impl_->config.role!=Role::Producer||impl_->auth.openControl({impl_->assembling.data(),impl_->assembling.size()},incoming.payload,incoming.channel)!=AuthStatus::Ok){impl_->close(ControlStatus::AuthenticationRefused);return;}
        impl_->verified[(impl_->rxHead+impl_->rxCount)%Impl::Capacity]=incoming;++impl_->rxCount;impl_->assembled=0;
    }
}
const char* controlStatusName(ControlStatus status) noexcept{
    switch(status){case ControlStatus::Disabled:return "control-disabled";case ControlStatus::WaitingVideo:return "waiting-for-authenticated-video";
    case ControlStatus::Listening:return "control-listening";case ControlStatus::Connecting:return "control-connecting";case ControlStatus::TransportConnected:return "control-tcp-connected-not-peer-confirmed";
    case ControlStatus::PairingRefused:return "control-pairing-refused";case ControlStatus::AddressRefused:return "control-address-refused";
    case ControlStatus::RandomRefused:return "control-random-source-refused";case ControlStatus::IoRefused:return "control-io-refused";
    case ControlStatus::AuthenticationRefused:return "control-authentication-refused";case ControlStatus::Lost:return "control-transport-lost";
    default:return "control-capacity-refused";}
}
} // namespace stream_bridge
