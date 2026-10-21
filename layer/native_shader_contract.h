#pragma once
#include "native_shader_contract_metadata.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
namespace stream_bridge_shader_contract {
struct Contract {const std::uint32_t* header;const std::uint32_t* resultIds;std::size_t resultCount;};
inline const Rule* rule(unsigned op,unsigned operands)noexcept {
    for(const auto& r:Rules)if(r.op==op&&r.operands==operands)return &r;
    return nullptr;
}
inline bool debug(unsigned op)noexcept{return op==0||(op>=2&&op<=8)||op==317||op==330;}
// Caller separately verifies the canonical module's exact reviewed digest.
inline bool normalize(const std::uint32_t* words,std::size_t bytes,Contract contract,
    std::uint32_t* output,std::size_t capacity,std::size_t& used)noexcept {
    used=0;
    if(!words||bytes<20||bytes>65536||bytes%4||capacity<5||words[0]!=0x07230203u||words[1]!=contract.header[1]||
       words[4]||!words[3]||words[3]>65536)return false;
    std::unique_ptr<std::uint32_t[]> ids(new(std::nothrow)std::uint32_t[words[3]]{});
    if(!ids)return false;
    std::size_t results=0;
    for(std::size_t at=5;at<bytes/4;){const auto n=words[at]>>16,op=words[at]&65535u;
        if(!n||n>bytes/4-at)return false;
        if(debug(op)){
            if(op==7){if(n<2||!words[at+1]||words[at+1]>=words[3]||ids[words[at+1]])return false;ids[words[at+1]]=0xffffffffu;}
        }else {
            const auto* r=rule(op,n-1);if(!r)return false;
            if(r->result>=0){const auto id=words[at+1+r->result];
                if(!id||id>=words[3]||ids[id]||results==contract.resultCount)return false;
                ids[id]=contract.resultIds[results++];}
        }at+=n;
    }
    if(results!=contract.resultCount)return false;
    std::memcpy(output,contract.header,20);used=5;
    for(std::size_t at=5;at<bytes/4;){const auto n=words[at]>>16,op=words[at]&65535u;
        if(!debug(op)){const auto* r=rule(op,n-1);if(n>capacity-used)return false;
            output[used++]=words[at];
            for(unsigned i=0;i<n-1;++i){auto value=words[at+1+i];
                if(r->idMask&(std::uint32_t(1)<<i)){
                    if(!value||value>=words[3]||!ids[value]||ids[value]==0xffffffffu)return false;
                    value=ids[value];}
                output[used++]=value;
            }
        }at+=n;
    }return true;
}
inline Contract vertex()noexcept{return {VertexHeader,VertexResultIds,sizeof(VertexResultIds)/sizeof(*VertexResultIds)};}
inline Contract fragment()noexcept{return {FragmentHeader,FragmentResultIds,sizeof(FragmentResultIds)/sizeof(*FragmentResultIds)};}
} // namespace stream_bridge_shader_contract
