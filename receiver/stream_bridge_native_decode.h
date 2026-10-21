#pragma once
#include "stream_bridge_native_elf.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/types.h>

namespace stream_bridge_native {
constexpr std::size_t MaxDevices=2,MaxBuffers=16;
// Cold 32 MiB reservation: retain one complete native OUTPUT-buffer burst.
// Slots are never evicted and no allocation occurs in decoder callbacks.
constexpr std::size_t SnapshotCapacity=MaxBuffers;
constexpr std::size_t MaxCodedBytes=2*1024*1024;
using Hash=std::array<std::uint8_t,32>;
extern const Hash ExpectedVrlinkHash;
extern const Hash ExpectedExecutableSegmentHash;
struct ImageIdentity{std::uintptr_t base=0;Hash file{},executableSegment{};NativeElfLayout layout{};bool publicAbiVerified=false;};
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
    std::uint32_t bufferCapacity=0;
};
enum class CaptureKind:std::uint8_t{Reset,Requeue,Decoded,SourceReset,InputGap};
enum class InputLoss:std::uint8_t{None,SnapshotFull,CopyRejected};
enum class ResetReason:std::uint8_t{None,OutputFormat,OutputBuffers,OutputStreamOff,
    CaptureFormat,CaptureBuffers,CaptureStreamOff,UnsupportedOutputQueue,
    UnsupportedCaptureQueue,OutputUnmap,DecoderClose,CaptureClose,ProviderStop};
const char* resetReasonName(ResetReason) noexcept;
struct CaptureObservation{
    CaptureKind kind=CaptureKind::Reset;int fd=-1,dmabufFd=-1;
    std::uint32_t index=0,nativeTimestampToken=0;
    std::uint64_t sourcePoolGeneration=0,capturePoolGeneration=0,contentGeneration=0;
    // Successful original OUTPUT QBUF with exact scalar ownership but no byte
    // snapshot. Sequence is the corresponding post-success loss counter, not
    // an AU certificate, CAPTURE lease or permission to select an image.
    InputLoss inputLoss=InputLoss::None;std::uint64_t lossSequence=0;
    // Cold worker diagnostics only; no per-frame logging or borrowed addresses.
    ResetReason resetReason=ResetReason::None;
};
struct SnapshotStats{std::uint64_t accepted=0,dropped=0,rejected=0,captureDropped=0;};
using CaptureSink=void(*)(const CaptureObservation&,void*) noexcept;
// Optional native decoder-thread handoff. Successful exact CAPTURE DQBUF only:
// after scalar publication and after releasing the provider mutex, before
// exposing the original successful result to native code. A registered owner
// can wait for an exact downstream coverage acknowledgement and may revalidate
// the lease through captureLeaseCurrentOnWorker. Never called by the compositor.
using CaptureHandoff=void(*)(const CaptureObservation&,void*) noexcept;
using ObservationWake=void(*)(void*) noexcept;

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
    bool nativeExportCaller(std::uintptr_t returnAddress)const noexcept;
    int invokeIoctl(std::uintptr_t returnAddress,int fd,unsigned long request,void* argument) noexcept;
    void* invokeMmap(std::uintptr_t returnAddress,void*,std::size_t,int,int,int,off_t) noexcept;
    int invokeMunmap(std::uintptr_t returnAddress,void*,std::size_t) noexcept;
    int invokeClose(std::uintptr_t returnAddress,int fd) noexcept;
    // Copies into caller-owned storage on its worker; no borrowed native bytes
    // outlive QBUF. Too-small outputs remain unchanged and keep the queued copy.
    bool poll(Observation&,std::uint8_t* output,std::size_t capacity) noexcept;
    bool pollCapture(CaptureObservation&) noexcept;
    bool captureLeaseCurrent(const CaptureObservation&) const noexcept;
    // Worker only: serialize the scalar check rather than mistake temporary
    // mutex contention for a stale lease. Never call this from rendering.
    bool captureLeaseCurrentOnWorker(const CaptureObservation&) const noexcept;
    bool setCaptureSink(CaptureSink,void*) noexcept;
    bool setCaptureHandoff(CaptureHandoff,void*) noexcept;
    bool setObservationWake(ObservationWake,void*) noexcept;
    SnapshotStats stats() const noexcept;
    OriginalIoctl originalIoctl() const noexcept;
    std::uintptr_t imageBase() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};

// Explicit preparation only. Does not patch a GOT, interpose libc, load itself,
// install a service, contact a device, or register a consumer. Exact original
// targets must be captured from the admitted process before any later hook.
bool preparePinnedProcess(const Originals&) noexcept;
bool preparedNativeExportCaller(std::uintptr_t)noexcept;
void stopPinnedProcess() noexcept;
bool pollPinnedProcess(Observation&,std::uint8_t*,std::size_t) noexcept;
bool pollPinnedCapture(CaptureObservation&) noexcept;
SnapshotStats pinnedSnapshotStats() noexcept;
bool pinnedCaptureLeaseCurrent(const CaptureObservation&) noexcept;
bool pinnedCaptureLeaseCurrentOnWorker(const CaptureObservation&) noexcept;
bool setPinnedCaptureSink(CaptureSink,void*) noexcept;
bool setPinnedCaptureHandoff(CaptureHandoff,void*) noexcept;
bool setPinnedObservationWake(ObservationWake,void*) noexcept;
struct Entries{void* ioctl=nullptr;OriginalMmap mmap=nullptr;OriginalMunmap munmap=nullptr;OriginalClose close=nullptr;};
Entries preparedEntries() noexcept;
std::uintptr_t preparedImageBase() noexcept;
} // namespace stream_bridge_native
