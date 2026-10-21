#pragma once
// Bounded PE/public-ABI discovery. Contains no native instruction bodies or
// per-build offsets: imports and MSVC's public interface RTTI locate bindings.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
namespace stream_bridge::sender {
struct NativeExecutableRange {std::uint32_t begin=0,size=0;};
struct NativeImageLayout {
    std::uint64_t preferredBase=0;
    std::uint32_t imageSize=0,getProcAddress=0,directTable=0;
    std::array<std::uint32_t,8> directMethods{};
    std::array<NativeExecutableRange,8> executable{};
    unsigned executableCount=0;
    bool containsCode(std::uint32_t rva)const noexcept {
        if(executableCount>executable.size())return false;
        for(unsigned i=0;i<executableCount;++i)if(rva>=executable[i].begin&&rva-executable[i].begin<executable[i].size)return true;
        return false;
    }
};
class NativePeImage {
    struct Section {std::uint32_t rva=0,size=0,raw=0,rawSize=0,flags=0;};
    const std::uint8_t* bytes_=nullptr;std::size_t count_=0;
    std::array<Section,16> sections_{};unsigned sectionCount_=0;
    std::uint64_t base_=0;std::uint32_t size_=0,imports_=0,importBytes_=0;
    bool bounds(std::size_t at,std::size_t n)const noexcept{return at<=count_&&n<=count_-at;}
    template<class T>bool number(std::size_t at,T& out)const noexcept {
        if(!bounds(at,sizeof(T)))return false;
        out=0;
        for(unsigned i=0;i<sizeof(T);++i)out|=T(bytes_[at+i])<<(8*i);
        return true;
    }
    template<class T>bool value(std::uint32_t rva,T& out)const noexcept {
        const auto* p=at(rva,sizeof(T));if(!p)return false;out=0;
        for(unsigned i=0;i<sizeof(T);++i)out|=T(p[i])<<(8*i);
        return true;
    }
    bool string(std::uint32_t rva,const char* text)const noexcept {
        const auto n=std::strlen(text)+1;const auto* p=at(rva,n);return p&&!std::memcmp(p,text,n);
    }
    bool nameContains(std::uint32_t rva,const char* part)const {
        if(rva>size_||size_-rva<160)return false;
        std::string text;for(unsigned i=0;i<160;++i){const auto* p=at(rva+i,1);if(!p)return false;if(!*p)return text.find(part)!=std::string::npos;text.push_back(char(*p));}return false;
    }
    bool interfaceLocator(std::uint32_t rva)const {
        std::uint32_t signature=0,offset=0,construction=0,type=0,hierarchy=0,self=0;
        if(!at(rva,24)||!value(rva,signature)||signature!=1||!value(rva+4,offset)||!value(rva+8,construction)||construction||
           !value(rva+12,type)||!value(rva+16,hierarchy)||!value(rva+20,self)||self!=rva||
           !at(type,16)||!nameContains(type+16,"D3D11")||!at(hierarchy,16))return false;
        std::uint32_t count=0,array=0;if(!value(hierarchy+8,count)||count<3||count>32||!value(hierarchy+12,array)||!at(array,count*4))return false;
        unsigned direct=0,tracked=0,display=0;
        for(unsigned i=0;i<count;++i){std::uint32_t b=0,t=0,member=0,virtualBase=0;
            if(!value(array+i*4,b)||!at(b,16)||!value(b,t)||!at(t,16)||!value(b+8,member)||!value(b+12,virtualBase))return false;
            if(string(t+16,".?AVIVRDriverDirectModeComponent@vr@@")&&member==offset&&virtualBase==0xffffffffu)++direct;
            if(string(t+16,".?AVITrackedDeviceServerDriver@vr@@"))++tracked;
            if(string(t+16,".?AVIVRDisplayComponent@vr@@"))++display;
        }
        return direct==1&&tracked==1&&display==1;
    }
public:
    bool open(const std::uint8_t* bytes,std::size_t n)noexcept {
        bytes_=bytes;count_=n;sectionCount_=0;
        std::uint16_t magic=0,machine=0,sections=0,optional=0;std::uint32_t pe=0,signature=0,dirs=0;
        if(!bytes||n<512||n>64*1024*1024||!number(0,magic)||magic!=0x5a4d||!number(60,pe)||pe>1024*1024||
           !number(pe,signature)||signature!=0x4550||!number(pe+4,machine)||machine!=0x8664||
           !number(pe+6,sections)||!sections||sections>sections_.size()||!number(pe+20,optional)||optional<240||
           !number(pe+24,magic)||magic!=0x20b||!number(pe+48,base_)||!number(pe+80,size_)||size_<4096||size_>64*1024*1024||
           !number(pe+132,dirs)||dirs<2||dirs>16||!number(pe+144,imports_)||!number(pe+148,importBytes_)||
           !imports_||imports_>size_||importBytes_<20||importBytes_>65536||importBytes_>size_-imports_||
           base_>std::numeric_limits<std::uint64_t>::max()-size_)return false;
        const auto table=std::size_t(pe)+24+optional;
        for(unsigned i=0;i<sections;++i){Section s;const auto h=table+i*40;
            if(!number(h+8,s.size)||!number(h+12,s.rva)||!number(h+16,s.rawSize)||!number(h+20,s.raw)||!number(h+36,s.flags)||
               !s.size||s.rva>size_||s.size>size_-s.rva||!bounds(s.raw,s.rawSize))return false;
            for(unsigned j=0;j<sectionCount_;++j){const auto& old=sections_[j];
                if(s.rva<old.rva+old.size&&old.rva<s.rva+s.size)return false;
                if(s.rawSize&&old.rawSize&&s.raw<old.raw+old.rawSize&&old.raw<s.raw+s.rawSize)return false;
            }
            sections_[sectionCount_++]=s;
        }
        return true;
    }
    const std::uint8_t* at(std::uint32_t rva,std::size_t n)const noexcept {
        for(unsigned i=0;i<sectionCount_;++i){const auto& s=sections_[i];
            if(rva>=s.rva&&rva-s.rva<=s.size&&n<=s.size-(rva-s.rva)&&rva-s.rva<=s.rawSize&&n<=s.rawSize-(rva-s.rva))return bytes_+s.raw+(rva-s.rva);
        }return nullptr;
    }
    bool executable(std::uint32_t rva)const noexcept {
        for(unsigned i=0;i<sectionCount_;++i){const auto&s=sections_[i];if((s.flags&0x20000000u)&&rva>=s.rva&&rva-s.rva<s.size)return true;}return false;
    }
    bool discover(NativeImageLayout& out,const char*& why)const {
        NativeImageLayout result;result.imageSize=size_;result.preferredBase=base_;
        for(unsigned i=0;i<sectionCount_;++i){const auto&s=sections_[i];if(s.flags&0x20000000u){
            if((s.flags&0x80000000u)||s.size>s.rawSize||result.executableCount==result.executable.size()){why="unsupported executable mapping";return false;}
            result.executable[result.executableCount++]={s.rva,s.size};}}
        if(!result.executableCount){why="native executable mapping missing";return false;}
        unsigned imports=0;bool terminated=false;
        for(unsigned i=0;i<256&&i*20+20<=importBytes_;++i){const auto r=imports_+i*20;std::uint32_t names=0,dll=0,slots=0;
            if(!value(r,names)||!value(r+12,dll)||!value(r+16,slots)){why="invalid import directory";return false;}
            if(!names&&!dll&&!slots){terminated=true;break;}
            if(!names||!slots||names>size_||slots>size_){why="unsupported bound import names";return false;}
            bool end=false;
            for(unsigned k=0;k<4096;++k){std::uint64_t item=0;if(k*8>size_-names||k*8>size_-slots||!at(slots+k*8,8)||!value(names+k*8,item)){why="invalid native import name";return false;}
                if(!item){end=true;break;}if(item>>63)continue;
                if(item>0xffffffffu-2){why="invalid native import RVA";return false;}
                if(string(std::uint32_t(item)+2,"GetProcAddress")){result.getProcAddress=slots+k*8;++imports;}
            }if(!end){why="native import bound exceeded";return false;}
        }
        if(!terminated||imports!=1||!at(result.getProcAddress,8)){why="public GetProcAddress import missing or ambiguous";return false;}
        // The versioned public interface must exist in the image. A new table
        // with additional methods does not silently inherit this ABI.
        const char version[]="IVRDriverDirectModeComponent_009";bool versionFound=false;
        for(unsigned i=0;i<sectionCount_;++i){const auto&s=sections_[i];if(s.flags&0x80000000u)continue;
            for(std::size_t k=0;k+sizeof(version)<=s.rawSize;++k)if(!std::memcmp(bytes_+s.raw+k,version,sizeof(version))){versionFound=true;break;}}
        if(!versionFound){why="DirectMode009 public interface missing";return false;}
        unsigned candidates=0;
        for(unsigned i=0;i<sectionCount_;++i){const auto&s=sections_[i];if(s.flags&(0x80000000u|0x20000000u))continue;
            for(std::uint32_t off=0;off+24<=s.rawSize&&off+24<=s.size;off+=4){const auto rva=s.rva+off;
                if(!interfaceLocator(rva))continue;
                for(unsigned j=0;j<sectionCount_;++j){const auto&t=sections_[j];if(t.flags&(0x80000000u|0x20000000u))continue;
                    for(std::uint32_t k=0;k+80<=t.rawSize&&k+80<=t.size;k+=8){std::uint64_t locator=0;if(!value(t.rva+k,locator)||locator!=base_+rva)continue;
                        std::array<std::uint32_t,8> methods{};bool okay=true;
                        for(unsigned m=0;m<8;++m){std::uint64_t pointer=0;
                            okay=okay&&value(t.rva+k+8+m*8,pointer)&&pointer>=base_&&pointer-base_<=0xffffffffu&&executable(std::uint32_t(pointer-base_));
                            if(!okay)break;
                            methods[m]=std::uint32_t(pointer-base_);}
                        std::uint64_t next=0;if(!okay||!value(t.rva+k+72,next)||(next>=base_&&next-base_<=0xffffffffu&&executable(std::uint32_t(next-base_))))continue;
                        result.directTable=t.rva+k+8;result.directMethods=methods;++candidates;
                    }
                }
            }
        }
        if(candidates!=1){why="D3D11 DirectMode009 table missing or ambiguous";return false;}
        out=result;why="public import and DirectMode009 RTTI capabilities verified";return true;
    }
};
} // namespace stream_bridge::sender
