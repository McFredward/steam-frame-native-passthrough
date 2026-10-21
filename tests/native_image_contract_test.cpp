// Entirely synthetic public PE/ELF fixtures; no Valve code or disassembly.
#include "../pc/stream-bridge-native-image.h"
#include "../receiver/stream_bridge_native_elf.h"
#include "../layer/native_shader_contract.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks=0;
void check(bool yes,const char* reason){++checks;if(!yes)throw std::runtime_error(reason);}
struct Fixture {
    std::vector<std::uint8_t> data=std::vector<std::uint8_t>(0x5000);
    void put(std::size_t at,std::uint64_t value,unsigned n=4){check(at+n<=data.size(),"fixture write bounded");for(unsigned i=0;i<n;++i)data[at+i]=std::uint8_t(value>>(i*8));}
    void text(std::size_t at,const char* value){const auto n=std::strlen(value)+1;check(at+n<=data.size(),"fixture string bounded");std::memcpy(data.data()+at,value,n);}
};
Fixture pe(unsigned shift=0){
    Fixture f;f.put(0,0x5a4d,2);f.put(60,128);f.put(128,0x4550);f.put(132,0x8664,2);f.put(134,2,2);f.put(148,240,2);
    f.put(152,0x20b,2);f.put(176,0x180000000ULL,8);f.put(208,0x5000);f.put(260,16);f.put(272,0x2100+shift);f.put(276,40);
    f.put(400,0x1000);f.put(404,0x1000);f.put(408,0x1000);f.put(412,0x1000);f.put(428,0x60000000);
    f.put(440,0x3000);f.put(444,0x2000);f.put(448,0x3000);f.put(452,0x2000);f.put(468,0x40000000);
    f.put(0x2100+shift,0x2200+shift);f.put(0x210c+shift,0x2300);f.put(0x2110+shift,0x2280+shift);
    f.put(0x2200+shift,0x2340,8);f.text(0x2300,"KERNEL32.dll");f.text(0x2342,"GetProcAddress");
    f.text(0x2380,"IVRDriverDirectModeComponent_009");
    f.put(0x2500+shift,1);f.put(0x2504+shift,16);f.put(0x250c+shift,0x2600);f.put(0x2510+shift,0x2700);f.put(0x2514+shift,0x2500+shift);
    f.text(0x2610,".?AVSyntheticD3D11@@");f.put(0x2708,3);f.put(0x270c,0x2780);
    const char* names[]{".?AVIVRDriverDirectModeComponent@vr@@",".?AVITrackedDeviceServerDriver@vr@@",".?AVIVRDisplayComponent@vr@@"};
    for(unsigned i=0;i<3;++i){f.put(0x2780+i*4,0x2800+i*32);f.put(0x2800+i*32,0x2900+i*160);f.put(0x2808+i*32,i?0:16);f.put(0x280c+i*32,0xffffffffu);f.text(0x2910+i*160,names[i]);}
    f.put(0x3000+shift,0x180002500ULL+shift,8);
    for(unsigned i=0;i<8;++i)f.put(0x3008+shift+i*8,0x180001100ULL+i*32+shift,8);
    return f;
}
bool discover(Fixture& f,stream_bridge::sender::NativeImageLayout& layout){stream_bridge::sender::NativePeImage parser;const char* why="";return parser.open(f.data.data(),f.data.size())&&parser.discover(layout,why);}
void peChecks(){
    for(unsigned shift:{0u,128u}){auto f=pe(shift);stream_bridge::sender::NativeImageLayout result;
        check(discover(f,result),"public PE discovery accepts shifted imports/RTTI/table/methods without a build hash");
        check(result.getProcAddress==0x2280+shift&&result.directTable==0x3008+shift&&result.directMethods[7]==0x11e0+shift,"derived bindings come from actual image");
        check(result.containsCode(0x1100)&&!result.containsCode(0x3008),"only executable ranges admit callers");
    }
    for(unsigned variant=0;variant<11;++variant){auto f=pe();stream_bridge::sender::NativeImageLayout result;
        if(variant==0)f.put(132,0xaa64,2);
        if(variant==1)f.text(0x2380,"IVRDriverDirectModeComponent_010");
        if(variant==2)f.text(0x2342,"DifferentImport");
        if(variant==3)f.put(0x3048,0x180001300ULL,8); // A ninth executable method is a different ABI.
        if(variant==4)f.put(0x250c,0xfffffff8u);
        if(variant==5)f.put(0x270c,0xfffffffcu);
        if(variant==6)f.put(0x2800,0xfffffff8u);
        if(variant==7)f.put(0x2100,0xfffffff8u);
        if(variant==8)f.put(428,0xe0000000u); // W+X.
        if(variant==9){for(unsigned i=0;i<80;++i)f.data[0x3100+i]=f.data[0x3000+i];}
        if(variant==10)f.put(0x3008,0x180003500ULL,8);
        check(!discover(f,result),"changed ABI, malformed bounds, foreign method or ambiguous table is refused");
    }
}
Fixture elf(unsigned shift=0){
    Fixture f;std::memcpy(f.data.data(),"\177ELF\2\1\1",7);f.put(16,3,2);f.put(18,183,2);f.put(32,64,8);f.put(54,56,2);f.put(56,3,2);
    f.put(64,1);f.put(68,5);f.put(72,0,8);f.put(80,0,8);f.put(96,0x1000,8);f.put(104,0x1000,8);
    f.put(120,1);f.put(124,6);f.put(128,0x1000,8);f.put(136,0x2000,8);f.put(152,0x4000,8);f.put(160,0x4000,8);
    f.put(176,2);f.put(184,0x1100,8);f.put(192,0x2100,8);f.put(208,128,8);
    const std::uint64_t tags[]{5,10,6,11,23,2,20,0},values[]{0x2300,128,0x2400,24,0x2500,96,7,0};
    for(unsigned i=0;i<8;++i){f.put(0x1100+i*16,tags[i],8);f.put(0x1108+i*16,values[i],8);}
    const char* names[]{"mmap","munmap","close","ioctl"};
    for(unsigned i=0;i<4;++i){f.text(0x1300+i*16,names[i]);f.put(0x1400+i*24,i*16);f.put(0x1500+i*24,0x2800+shift+i*8,8);f.put(0x1508+i*24,(std::uint64_t(i)<<32)|1026,8);}
    return f;
}
bool discoverElf(Fixture& f,stream_bridge_native::NativeElfLayout& layout,bool decoder=true){stream_bridge_native::NativeElfImage parser;return parser.open(f.data.data(),f.data.size(),decoder,layout);}
void elfChecks(){
    for(unsigned shift:{0u,256u}){auto f=elf(shift);stream_bridge_native::NativeElfLayout result;
        check(discoverElf(f,result),"public V4L2 imports accept changed ELF bytes and moved GOT slots");
        check(result.imports[3]==0x2818+shift&&result.caller(0x100000,0x100800)&&!result.caller(0x100000,0x102800),"discovered imports and RX callers are exact");
        check(discoverElf(f,result,false),"compositor structural identity needs no decoder imports");
    }
    for(unsigned variant=0;variant<8;++variant){auto f=elf();stream_bridge_native::NativeElfLayout result;
        if(variant==0)f.put(18,62,2);
        if(variant==1)f.put(32,UINT64_MAX-16,8);
        if(variant==2)f.put(68,7);
        if(variant==3)f.text(0x1330,"other");
        if(variant==4)f.put(0x1500,0x800,8);
        if(variant==5)f.put(0x1508,UINT64_MAX,8);
        if(variant==6)f.put(0x1108+2*16,UINT64_MAX-16,8);
        if(variant==7)f.put(0x1500,0x2801,8);
        check(!discoverElf(f,result),"wrong architecture, writable code, missing imports or invalid relocation refuses admission");
    }
}
void shaderChecks(){
    namespace s=stream_bridge_shader_contract;
    // Own tiny declaration fixture: TypeFloat result and Constant literal have
    // deliberately identical numbers. Only ID operands may be renumbered.
    const std::uint32_t header[]{0x07230203,0x10000,0,8,0},results[]{2,3};
    const s::Contract contract{header,results,2};std::uint32_t output[64]{};std::size_t used=0;
    const std::uint32_t input[]{0x07230203,0x10000,991,32,0,(3u<<16)|22,19,32,(4u<<16)|43,19,20,19};
    check(s::normalize(input,sizeof input,contract,output,64,used)&&used==12,"canonicalization accepts changed compiler metadata and IDs");
    check(output[6]==2&&output[9]==2&&output[10]==3&&output[11]==19,"literal is unchanged even when its value equals an input ID");
    std::vector<std::uint32_t> debug(input,input+12);debug.insert(debug.begin()+5,{(3u<<16)|5,19,0x006f6f66});
    check(s::normalize(debug.data(),debug.size()*4,contract,output,64,used),"debug names do not change shader logic");
    debug.assign(input,input+12);
    debug[10]=19;check(!s::normalize(debug.data(),debug.size()*4,contract,output,64,used),"duplicate result IDs are refused");
    debug.assign(input,input+12);debug[9]=31;check(!s::normalize(debug.data(),debug.size()*4,contract,output,64,used),"undefined executable dependency is refused");
}
}
int main(){try{peChecks();elfChecks();shaderChecks();std::printf("public native image and shader contracts: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"contract failure after %u: %s\n",checks,e.what());return 1;}}
