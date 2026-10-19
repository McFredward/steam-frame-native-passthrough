#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stream_bridge_native_loader.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>
namespace stream_bridge_native {
bool publishSlots(const std::array<ImportSlot,4>& slots,Exchange exchange,void* context) noexcept{
    if(!exchange)return false;
    for(std::size_t i=0;i<slots.size();++i){const auto& s=slots[i];
        if(!s.address||reinterpret_cast<std::uintptr_t>(s.address)%alignof(std::uintptr_t)||!s.expected||!s.replacement||s.expected==s.replacement)return false;
        for(std::size_t j=0;j<i;++j)if(s.address==slots[j].address)return false;
    }
    std::size_t done=0;
    for(;done<slots.size();++done){auto wanted=slots[done].expected;
        if(!exchange(slots[done].address,wanted,slots[done].replacement,context))break;}
    if(done==slots.size())return true;
    while(done){const auto& s=slots[--done];auto owned=s.replacement;exchange(s.address,owned,s.expected,context);}
    return false;
}
void restoreSlots(const std::array<ImportSlot,4>& slots,Exchange exchange,void* context) noexcept{
    if(!exchange)return;
    for(auto it=slots.rbegin();it!=slots.rend();++it)if(it->address){auto owned=it->replacement;exchange(it->address,owned,it->expected,context);}
}
namespace {
std::mutex gate;std::array<ImportSlot,4> installed{};std::atomic<bool> published{false};
[[maybe_unused]] constexpr std::array<std::uintptr_t,4> Rvas{{0x24e070,0x24e110,0x24e8b0,0x24ec58}};
bool exchange(std::uintptr_t* address,std::uintptr_t& expected,std::uintptr_t value,void*) noexcept{
    return __atomic_compare_exchange_n(address,&expected,value,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
template<class T>std::uintptr_t bits(T value) noexcept{static_assert(sizeof(T)==sizeof(std::uintptr_t),"LP64 imports");std::uintptr_t out=0;std::memcpy(&out,&value,sizeof out);return out;}
template<class T>T function(void* value) noexcept{static_assert(sizeof(T)==sizeof(void*),"LP64 imports");T out{};std::memcpy(&out,&value,sizeof out);return out;}
struct Main{std::uintptr_t base=0,begin=0,end=0;};
[[maybe_unused]] int mainImage(dl_phdr_info* info,std::size_t,void* context){
    if(info->dlpi_name&&*info->dlpi_name)return 0;
    auto& out=*static_cast<Main*>(context);out.base=info->dlpi_addr;
    for(unsigned i=0;i<info->dlpi_phnum;++i){const auto& p=info->dlpi_phdr[i];
        if(p.p_type==PT_LOAD&&p.p_flags==(PF_R|PF_W)&&p.p_vaddr==0x2489d8&&p.p_memsz==0xa4f1c8){
            out.begin=out.base+p.p_vaddr;out.end=out.begin+p.p_memsz;}}
    return 1;
}
[[maybe_unused]] bool writable(std::uintptr_t pointer) noexcept{
    auto* file=std::fopen("/proc/self/maps","r");if(!file)return false;
    char line[1024];bool okay=false;unsigned count=0;
    while(std::fgets(line,sizeof line,file)&&++count<=4096){unsigned long begin=0,end=0;char flags[5]{};
        if(std::sscanf(line,"%lx-%lx %4s",&begin,&end,flags)==3&&pointer>=begin&&pointer<=end-sizeof(std::uintptr_t)){
            okay=flags[0]=='r'&&flags[1]=='w'&&flags[2]=='-'&&flags[3]=='p';break;}}
    std::fclose(file);return okay;
}
}
bool publishPinnedDecodeImports() noexcept{
    std::lock_guard<std::mutex> lock(gate);if(published.load(std::memory_order_acquire))return true;
#if !defined(__aarch64__)
    return false;
#else
    Main main{};dl_iterate_phdr(mainImage,&main);if(!main.base||!main.begin||!main.end)return false;
    std::array<void*,4> originals{};const char* names[] {"mmap","munmap","close","ioctl"};
    std::array<ImportSlot,4> candidate{};
    for(std::size_t i=0;i<candidate.size();++i){
        originals[i]=dlvsym(RTLD_NEXT,names[i],"GLIBC_2.17");if(!originals[i])return false;
        const auto address=main.base+Rvas[i];if(address<main.begin||address>main.end-sizeof(std::uintptr_t)||!writable(address))return false;
        const auto value=__atomic_load_n(reinterpret_cast<std::uintptr_t*>(address),__ATOMIC_ACQUIRE);
        if(value!=main.base+0x1a450&&value!=bits(originals[i]))return false;
        candidate[i]={reinterpret_cast<std::uintptr_t*>(address),value,0};
    }
    const Originals calls{function<OriginalIoctl>(originals[3]),function<OriginalMmap>(originals[0]),
        function<OriginalMunmap>(originals[1]),function<OriginalClose>(originals[2])};
    if(!preparePinnedProcess(calls)||preparedImageBase()!=main.base)return false;
    const auto entries=preparedEntries();candidate[0].replacement=bits(entries.mmap);candidate[1].replacement=bits(entries.munmap);
    candidate[2].replacement=bits(entries.close);candidate[3].replacement=bits(entries.ioctl);
    if(!publishSlots(candidate,exchange,nullptr)){stopPinnedProcess();return false;}
    installed=candidate;published.store(true,std::memory_order_release);return true;
#endif
}
void restorePinnedDecodeImports() noexcept{
    std::lock_guard<std::mutex> lock(gate);if(!published.exchange(false,std::memory_order_acq_rel))return;
    stopPinnedProcess();restoreSlots(installed,exchange,nullptr);
}
bool decodeImportsPublished() noexcept{return published.load(std::memory_order_acquire);}
}
