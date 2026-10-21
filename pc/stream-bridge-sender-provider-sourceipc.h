#pragma once
#include "bridge/stream_bridge_protocol.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
namespace stream_bridge::sender {
constexpr std::size_t SourceIpcBytes=128;
using SourceIpcMessage=std::array<std::uint8_t,SourceIpcBytes>;
struct SourceIpcRequest {std::uint32_t processId=0;Nonce nonce{};bool metadataBudget=false;};
inline bool readSourceRequest(const SourceIpcMessage& input,std::uint32_t kernelClientPid,SourceIpcRequest& output)noexcept{
    auto u16=[&](unsigned p){return unsigned(input[p])|(unsigned(input[p+1])<<8);};
    auto u32=[&](unsigned p){return std::uint32_t(input[p])|(std::uint32_t(input[p+1])<<8)|(std::uint32_t(input[p+2])<<16)|(std::uint32_t(input[p+3])<<24);};
    if(std::memcmp(input.data(),"SBN1",4)||u16(4)!=1||(u16(6)!=1&&u16(6)!=2)||u16(8)!=128||u16(10)||u32(12)!=1||!kernelClientPid||u32(16)!=kernelClientPid||u32(20))return false;
    for(unsigned i=40;i<128;++i)if(input[i])return false;
    SourceIpcRequest local;local.processId=kernelClientPid;local.metadataBudget=u16(6)==2;bool any=false;for(unsigned i=0;i<16;++i){local.nonce[i]=input[24+i];any|=local.nonce[i]!=0;}if(!any)return false;output=local;return true;
}
inline SourceIpcMessage sourceReply(const SourceIpcRequest& request,std::uint32_t serverPid,const Nonce& binding,std::uint64_t generation,bool canAccept)noexcept{
    SourceIpcMessage out{};std::memcpy(out.data(),"SBN1",4);
    auto w16=[&](unsigned at,std::uint16_t v){for(unsigned i=0;i<2;++i)out[at+i]=std::uint8_t(v>>(8*i));};
    auto w32=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)out[at+i]=std::uint8_t(v>>(8*i));};
    w16(4,2);w16(6,request.metadataBudget?2:1);w16(8,128);w32(12,1);w32(16,request.processId);w32(20,serverPid);std::memcpy(out.data()+24,request.nonce.data(),16);
    bool any=false;for(auto b:binding)any|=b!=0;
    if(canAccept&&generation&&any){w16(10,request.metadataBudget?5:1);std::memcpy(out.data()+40,binding.data(),16);for(unsigned i=0;i<8;++i)out[56+i]=std::uint8_t(generation>>(8*i));}
    // flag2 is deliberately absent: sending a capability reply is not proof
    // that the remote selected image actually consumed its coverage.
    return out;
}
struct SourceIpcState {Nonce binding{};std::uint64_t generation=0;bool compatible=false;};
// A source-format lease belongs to one already kernel-validated local pipe
// client. It survives encoder/pool renegotiation, but never grants image or
// coverage authority; those still require the fresh exact native frame proof.
class SourceIpcPermission {
public:
    void reset()noexcept{client_={};server_=0;binding_={};generation_=0;}
    std::uint32_t process()const noexcept{return generation_?client_.processId:0;}
    bool metadataBudget()const noexcept{return generation_&&client_.metadataBudget;}
    SourceIpcMessage update(const SourceIpcRequest& client,std::uint32_t server,
        const SourceIpcState& current,bool prepared)noexcept{
        if(client.processId!=client_.processId||client.nonce!=client_.nonce||client.metadataBudget!=client_.metadataBudget||server!=server_){
            reset();client_=client;server_=server;
        }
        bool nonce=false,binding=false;for(auto v:client.nonce)nonce|=v!=0;for(auto v:current.binding)binding|=v!=0;
        if(!generation_&&client.processId&&server&&nonce&&current.compatible&&prepared&&current.generation&&binding){
            binding_=current.binding;generation_=current.generation;
        }
        return sourceReply(client,server,binding_,generation_,generation_!=0);
    }
private:
    SourceIpcRequest client_{};std::uint32_t server_=0;Nonce binding_{};std::uint64_t generation_=0;
};
class SourceIpcServer {
public:
    using Prepared=bool(*)(std::uint32_t,void*);
    using Changed=void(*)(std::uint32_t,bool,void*) noexcept;
    explicit SourceIpcServer(Prepared,void*,Changed=nullptr);~SourceIpcServer();
    void tick(const SourceIpcState&)noexcept; // Existing cold provider worker only.
    void stop()noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge::sender
