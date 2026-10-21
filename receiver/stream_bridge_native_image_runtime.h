#pragma once
#include "stream_bridge_native_elf.h"
#include "sha256.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
namespace stream_bridge_native {
enum class NativeRole {None,Vrlink,Compositor};
inline const char* roleSuffix(NativeRole role)noexcept {
    return role==NativeRole::Vrlink?"/tools/vrlink/bin/linuxarm64/vrlink":role==NativeRole::Compositor?"/bin/linuxarm64/vrcompositor":"";
}
inline std::string processImagePath(pid_t pid){
    char name[64],path[4096];std::snprintf(name,sizeof(name),"/proc/%d/exe",pid);const auto n=readlink(name,path,sizeof(path));
    if(n<=0||std::size_t(n)==sizeof(path))return {};
    std::string result(path,n),deleted=" (deleted)";
    if(result.size()>deleted.size()&&result.compare(result.size()-deleted.size(),deleted.size(),deleted)==0)result.resize(result.size()-deleted.size());
    return result;
}
inline NativeRole nativeImageRole(const std::string& path,std::string* root=nullptr){
    for(auto role:{NativeRole::Vrlink,NativeRole::Compositor}){const std::string suffix=roleSuffix(role);
        if(path.size()>suffix.size()&&path.compare(path.size()-suffix.size(),suffix.size(),suffix)==0){if(root)*root=path.substr(0,path.size()-suffix.size());return role;}}
    return NativeRole::None;
}
inline bool readNativeFile(const char* path,std::vector<std::uint8_t>& data){
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);if(fd<0)return false;
    struct stat before{},after{};bool okay=!fstat(fd,&before)&&S_ISREG(before.st_mode)&&before.st_size>=64&&before.st_size<=64*1024*1024;
    if(okay){try{data.resize(std::size_t(before.st_size));}catch(...){close(fd);throw;}std::size_t total=0;
        while(total<data.size()){const auto n=read(fd,data.data()+total,data.size()-total);if(n<0&&errno==EINTR)continue;if(n<=0){okay=false;break;}total+=std::size_t(n);}
        okay=okay&&!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&
            before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec;
    }close(fd);return okay;
}
struct RuntimeNativeImage {std::uintptr_t base=0;NativeElfLayout layout{};std::array<std::uint8_t,32> file{},code{};};
inline bool currentNativeImage(NativeRole role,RuntimeNativeImage& out)noexcept{
    try{
#if !defined(__aarch64__)
    (void)role;(void)out;return false;
#else
    if(nativeImageRole(processImagePath(getpid()))!=role)return false;
    std::vector<std::uint8_t> raw;if(!readNativeFile("/proc/self/exe",raw))return false;
    NativeElfImage elf;NativeElfLayout layout;if(!elf.open(raw.data(),raw.size(),role==NativeRole::Vrlink,layout))return false;
    struct Main {std::uintptr_t base=0;bool found=false;};Main main;
    dl_iterate_phdr([](dl_phdr_info* info,std::size_t,void* context){
        if(info->dlpi_name&&*info->dlpi_name)return 0;
        auto& value=*static_cast<Main*>(context);value.base=info->dlpi_addr;value.found=true;return 1;
    },&main);
    if(!main.found||!main.base)return false;
    frame_receiver::Sha256 fileHash;fileHash.update(raw.data(),raw.size());
    frame_receiver::Sha256 codeHash;
    for(unsigned i=0;i<layout.codeCount;++i){const auto& range=layout.code[i];const auto* file=elf.at(range.offset,range.bytes);if(!file||range.offset>UINTPTR_MAX-main.base||range.bytes>UINTPTR_MAX-main.base-range.offset)return false;
        std::vector<std::uint8_t> actual(range.bytes);iovec local{actual.data(),actual.size()},remote{reinterpret_cast<void*>(main.base+range.offset),actual.size()};
        if(process_vm_readv(getpid(),&local,1,&remote,1,0)!=static_cast<ssize_t>(actual.size())||std::memcmp(actual.data(),file,actual.size()))return false;
        codeHash.update(actual.data(),actual.size());
    }
    out={main.base,layout,fileHash.finish(),codeHash.finish()};return true;
#endif
    }catch(...){return false;}
}
// The current runtime root and SO_PEERCRED process path select the intended
// peer. Hashes remain observations, rather than a compatibility allowlist.
inline bool compatibleRuntimePeer(pid_t pid,NativeRole role,const std::string& root){
    if(pid<=0||root.empty()||processImagePath(pid)!=root+roleSuffix(role))return false;
    char path[64];std::snprintf(path,sizeof(path),"/proc/%d/exe",pid);std::vector<std::uint8_t> bytes;
    if(!readNativeFile(path,bytes))return false;
    NativeElfImage elf;NativeElfLayout layout;return elf.open(bytes.data(),bytes.size(),role==NativeRole::Vrlink,layout);
}
} // namespace stream_bridge_native
