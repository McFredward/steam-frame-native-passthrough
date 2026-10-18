#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/types.h>

namespace stream_bridge_native {
constexpr std::size_t MaxDevices=2,MaxBuffers=16,SnapshotCapacity=2;
constexpr std::size_t MaxCodedBytes=2*1024*1024;
using Hash=std::array<std::uint8_t,32>;
extern const Hash ExpectedVrlinkHash;
extern const Hash ExpectedExecutableSegmentHash;
struct ImageIdentity{std::uintptr_t base=0;Hash file{},executableSegment{};};
struct Reader{
    bool (*copy)(void* output,std::uintptr_t source,std::size_t bytes,void*) noexcept=nullptr;
    void* context=nullptr;
};
using OriginalIoctl=int(*)(int,unsigned long,...);
using OriginalMmap=void*(*)(void*,std::size_t,int,int,int,off_t);
using OriginalMunmap=int(*)(void*,std::size_t);
using OriginalClose=int(*)(int);
struct Originals{OriginalIoctl ioctl=nullptr;OriginalMmap mmap=nullptr;OriginalMunmap munmap=nullptr;OriginalClose close=nullptr;};
struct Observation{
    int fd=-1;std::uint32_t index=0,usedBytes=0;
    std::uint64_t poolGeneration=0,contentGeneration=0;
    std::uint32_t nativeTimestampToken=0;
    // A V4L2 OUTPUT chunk is NOT by itself a proven complete HEVC access unit.
    bool completeAccessUnit=false;
};
struct SnapshotStats{std::uint64_t accepted=0,dropped=0,rejected=0;};

// Public V4L2 lifecycle observer. No private C++ object fields, metadata parser,
// authentication, renderer, settings or transport. Each original runs once.
// Actual runtime must call preparePinnedProcess before publishing any entry.
class DecodeProvider{
public:
    DecodeProvider();~DecodeProvider();
    DecodeProvider(const DecodeProvider&)=delete;
    DecodeProvider& operator=(const DecodeProvider&)=delete;
    // Caller boundary for the isolated core; real preparePinnedProcess computes
    // these identities from /proc/self/exe and the main ELF executable segment.
    bool prepare(const ImageIdentity&,const Originals&,Reader) noexcept;
    void stop() noexcept;
    bool knownIoctl(std::uintptr_t returnAddress,unsigned long request) const noexcept;
    int invokeIoctl(std::uintptr_t returnAddress,int fd,unsigned long request,void* argument) noexcept;
    void* invokeMmap(std::uintptr_t returnAddress,void*,std::size_t,int,int,int,off_t) noexcept;
    int invokeMunmap(std::uintptr_t returnAddress,void*,std::size_t) noexcept;
    int invokeClose(std::uintptr_t returnAddress,int fd) noexcept;
    // Copies into caller-owned storage on its worker; no borrowed native bytes
    // outlive QBUF. Too-small outputs remain unchanged and keep the queued copy.
    bool poll(Observation&,std::uint8_t* output,std::size_t capacity) noexcept;
    SnapshotStats stats() const noexcept;
    OriginalIoctl originalIoctl() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};

// Explicit preparation only. Does not patch a GOT, interpose libc, load itself,
// install a service, contact a device, or register a consumer. Exact original
// targets must be captured from the admitted process before any later hook.
bool preparePinnedProcess(const Originals&) noexcept;
void stopPinnedProcess() noexcept;
bool pollPinnedProcess(Observation&,std::uint8_t*,std::size_t) noexcept;
struct Entries{void* ioctl=nullptr;OriginalMmap mmap=nullptr;OriginalMunmap munmap=nullptr;OriginalClose close=nullptr;};
Entries preparedEntries() noexcept;
} // namespace stream_bridge_native
