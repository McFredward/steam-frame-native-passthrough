#pragma once
// Only public ELF64/AArch64 program/dynamic/import contracts are recognized.
// No build IDs, executable-wide digest allowlist or private instruction bytes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace stream_bridge_native {
struct NativeCodeRange {std::uint64_t offset=0,bytes=0;};
struct NativeLoadRange {std::uint64_t offset=0,bytes=0,fileOffset=0,fileBytes=0;std::uint32_t flags=0;};
struct NativeElfLayout {
    std::array<NativeLoadRange,16> loads{};unsigned loadCount=0;
    std::array<NativeCodeRange,8> code{};unsigned codeCount=0;
    std::array<std::uint64_t,4> imports{}; // mmap, munmap, close, ioctl
    bool caller(std::uintptr_t base,std::uintptr_t address)const noexcept {
        if(address<base||(address&3)||codeCount>code.size())return false;
        const auto offset=address-base;
        for(unsigned i=0;i<codeCount;++i)if(offset>=code[i].offset&&offset-code[i].offset<code[i].bytes)return true;
        return false;
    }
    bool writable(std::uint64_t address,std::size_t n)const noexcept{
        if(loadCount>loads.size())return false;
        for(unsigned i=0;i<loadCount;++i){const auto&p=loads[i];if((p.flags&3)==2&&address>=p.offset&&address-p.offset<=p.bytes&&n<=p.bytes-(address-p.offset))return true;}return false;
    }
};
class NativeElfImage {
    const std::uint8_t* data_=nullptr;std::size_t count_=0;NativeElfLayout layout_{};
    bool span(std::uint64_t at,std::uint64_t n)const noexcept{return at<=count_&&n<=count_-at;}
    template<class T>bool number(std::uint64_t at,T& out)const noexcept {
        if(!span(at,sizeof(T)))return false;
        out=0;
        for(unsigned i=0;i<sizeof(T);++i)out|=T(data_[at+i])<<(8*i);
        return true;
    }
    bool symbol(const std::uint8_t* strings,std::uint64_t size,std::uint32_t name,const char* expected)const noexcept {
        const auto n=std::strlen(expected)+1;return name<=size&&n<=size-name&&!std::memcmp(strings+name,expected,n);
    }
public:
    const std::uint8_t* at(std::uint64_t rva,std::uint64_t n)const noexcept{
        for(unsigned i=0;i<layout_.loadCount;++i){const auto&p=layout_.loads[i];
            if(rva>=p.offset&&rva-p.offset<=p.fileBytes&&n<=p.fileBytes-(rva-p.offset))return data_+p.fileOffset+(rva-p.offset);
        }return nullptr;
    }
    bool open(const std::uint8_t* data,std::size_t n,bool decoder,NativeElfLayout& result)noexcept {
        data_=data;count_=n;layout_={};
        if(!data||n<64||n>64*1024*1024||std::memcmp(data,"\177ELF\2\1\1",7))return false;
        std::uint16_t type=0,machine=0,entrySize=0,entryCount=0;std::uint64_t table=0;
        if(!number(16,type)||type!=3||!number(18,machine)||machine!=183||!number(32,table)||!number(54,entrySize)||entrySize!=56||
           !number(56,entryCount)||!entryCount||entryCount>64||!span(table,std::uint64_t(entryCount)*56))return false;
        std::uint64_t dynamic=0,dynamicBytes=0;
        for(unsigned i=0;i<entryCount;++i){const auto h=table+i*56;std::uint32_t kind=0,flags=0;NativeLoadRange p;
            if(!number(h,kind)||!number(h+4,flags)||!number(h+8,p.fileOffset)||!number(h+16,p.offset)||!number(h+32,p.fileBytes)||!number(h+40,p.bytes))return false;
            p.flags=flags;
            if(kind==2){if(dynamic||p.fileBytes>1024*1024)return false;dynamic=p.fileOffset;dynamicBytes=p.fileBytes;}
            if(kind!=1)continue;
            if(layout_.loadCount==layout_.loads.size()||!p.bytes||p.offset>512*1024*1024||p.bytes>512*1024*1024-p.offset||p.fileBytes>p.bytes||!span(p.fileOffset,p.fileBytes)||(flags&3)==3)return false;
            for(unsigned j=0;j<layout_.loadCount;++j){const auto&q=layout_.loads[j];if(p.offset<q.offset+q.bytes&&q.offset<p.offset+p.bytes)return false;}
            layout_.loads[layout_.loadCount++]=p;
            if(flags&1){if(!(flags&4)||layout_.codeCount==layout_.code.size()||p.fileBytes!=p.bytes)return false;
                layout_.code[layout_.codeCount++]={p.offset,p.bytes};}
        }
        if(!layout_.codeCount||!dynamic||dynamicBytes<16||dynamicBytes%16||!span(dynamic,dynamicBytes))return false;
        if(!decoder){result=layout_;return true;}
        std::uint64_t strings=0,stringBytes=0,symbols=0,symbolBytes=0,plt=0,pltBytes=0,pltKind=0,rela=0,relaBytes=0;
        bool terminated=false;
        for(std::uint64_t i=0;i<dynamicBytes;i+=16){std::uint64_t tag=0,value=0;if(!number(dynamic+i,tag)||!number(dynamic+i+8,value))return false;
            if(!tag){terminated=true;break;}
            if(tag==5)strings=value;else if(tag==10)stringBytes=value;else if(tag==6)symbols=value;else if(tag==11)symbolBytes=value;
            else if(tag==23)plt=value;else if(tag==2)pltBytes=value;else if(tag==20)pltKind=value;else if(tag==7)rela=value;else if(tag==8)relaBytes=value;
        }
        const auto* str=at(strings,stringBytes);
        if(!terminated||!str||!stringBytes||stringBytes>8*1024*1024||symbolBytes!=24||!symbols||!at(symbols,24)||pltKind!=7||!plt||!pltBytes||pltBytes>2*1024*1024||pltBytes%24)return false;
        const char* names[]{"mmap","munmap","close","ioctl"};
        for(unsigned part=0;part<2;++part){const auto start=part?rela:plt,size=part?relaBytes:pltBytes;
            if(!size)continue;
            if(size>2*1024*1024||size%24)return false;
            const auto* entries=at(start,size);if(!entries)return false;
            for(std::uint64_t i=0;i<size;i+=24){std::uint64_t offset=0,info=0;
                for(unsigned j=0;j<8;++j){offset|=std::uint64_t(entries[i+j])<<(j*8);info|=std::uint64_t(entries[i+8+j])<<(j*8);}
                const auto kind=std::uint32_t(info);const auto index=info>>32;
                if(kind!=1026&&kind!=1025)continue;
                if(index>1024*1024||symbols>UINT64_MAX-index*24)return false;
                const auto* sym=at(symbols+index*24,24);if(!sym)return false;
                std::uint32_t name=0;for(unsigned j=0;j<4;++j)name|=std::uint32_t(sym[j])<<(8*j);
                for(unsigned j=0;j<4;++j)if(symbol(str,stringBytes,name,names[j])){
                    if(!offset||(offset&7)||!layout_.writable(offset,8)||
                       (layout_.imports[j]&&layout_.imports[j]!=offset))return false;
                    layout_.imports[j]=offset;
                }
            }
        }
        for(auto entry:layout_.imports)if(!entry)return false;
        result=layout_;return true;
    }
};
} // namespace stream_bridge_native
