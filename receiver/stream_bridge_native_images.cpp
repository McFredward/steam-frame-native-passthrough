#include "stream_bridge_native_images.h"
#include "stream_bridge_native_loader.h"
#include <array>
#include <limits>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace stream_bridge_native {
namespace {
constexpr std::uint32_t DmaBuf=0x200;
bool prepared(std::uintptr_t caller,void*) noexcept {
    const auto base=preparedImageBase();return base&&decodeImportsPublished()&&caller==base+0x34574;
}
bool identity(int fd,std::uint64_t& device,std::uint64_t& inode) noexcept {
    struct stat value{};if(fd<0||fstat(fd,&value)!=0||!value.st_ino)return false;
    device=static_cast<std::uint64_t>(value.st_dev);inode=static_cast<std::uint64_t>(value.st_ino);return true;
}
bool captureShape(const CaptureObservation& c){return c.kind==CaptureKind::Decoded&&c.fd>=0&&c.dmabufFd>=0
    &&c.index<MaxBuffers&&c.nativeTimestampToken<=0xffff&&c.sourcePoolGeneration&&c.capturePoolGeneration&&c.contentGeneration;}
}
struct NativeImages::Impl {
    struct Image {DeviceHandle device=0;ImageHandle handle=0;std::uint64_t generation=0;ImageDescription description{};
        MemoryHandle memory=0;std::uint64_t memoryGeneration=0,offset=0;};
    struct Memory {DeviceHandle device=0;MemoryHandle handle=0;std::uint64_t generation=0;bool dmaBuf=false;};
    struct Export {DeviceHandle device=0;MemoryHandle memory=0;std::uint64_t memoryGeneration=0,imageGeneration=0;
        std::uint64_t deviceIdentity=0,inodeIdentity=0;int owned=-1;};
    mutable std::mutex mutex;std::array<Image,Capacity> images{};std::array<Memory,Capacity> memories{};std::array<Export,Capacity> exports{};
    ExportPreparation preparation=nullptr;void* context=nullptr;std::uint64_t next=0;bool healthy=true;
    std::uint64_t generation(){if(next==std::numeric_limits<std::uint64_t>::max()){healthy=false;return 0;}return ++next;}
    Image* image(DeviceHandle d,ImageHandle i){for(auto& entry:images)if(entry.device==d&&entry.handle==i&&i)return &entry;return nullptr;}
    Memory* memory(DeviceHandle d,MemoryHandle m){for(auto& entry:memories)if(entry.device==d&&entry.handle==m&&m)return &entry;return nullptr;}
    void retire(MemoryHandle m,std::uint64_t generation,DeviceHandle d){for(auto& e:exports)if(e.memory==m&&e.memoryGeneration==generation&&e.device==d){if(e.owned>=0)close(e.owned);e={};}}
    void reset(){for(auto& e:exports){if(e.owned>=0)close(e.owned);e={};}images={};memories={};healthy=true;}
};
NativeImages::NativeImages():NativeImages(prepared,nullptr){}
NativeImages::NativeImages(ExportPreparation p,void* context):impl_(new Impl){impl_->preparation=p;impl_->context=context;}
NativeImages::~NativeImages(){reset();}
bool NativeImages::created(DeviceHandle d,ImageHandle i,const ImageDescription& description) noexcept {
    if(!d||!i||!description.dmaBuf||!description.width||!description.height||description.width>16384||description.height>16384||!description.format)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->healthy||impl_->image(d,i))return false;
    for(auto& entry:impl_->images)if(!entry.handle){const auto generation=impl_->generation();if(!generation)return false;
        entry={d,i,generation,description,0,0,0};return true;}impl_->healthy=false;return false;
}
bool NativeImages::allocated(DeviceHandle d,MemoryHandle m,bool dmaBuf) noexcept {
    if(!d||!m||!dmaBuf)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->healthy||impl_->memory(d,m))return false;
    for(auto& entry:impl_->memories)if(!entry.handle){const auto generation=impl_->generation();if(!generation)return false;
        entry={d,m,generation,dmaBuf};return true;}impl_->healthy=false;return false;
}
bool NativeImages::bound(DeviceHandle d,ImageHandle i,MemoryHandle m,std::uint64_t offset) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);auto* image=impl_->image(d,i);auto* memory=impl_->memory(d,m);
    if(!impl_->healthy||!image||!memory||image->memory)return false;
    image->memory=m;image->memoryGeneration=memory->generation;image->offset=offset;return true;
}
void NativeImages::destroyed(DeviceHandle d,ImageHandle i) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);if(auto* image=impl_->image(d,i)){
        impl_->retire(image->memory,image->memoryGeneration,d);*image={};}
}
void NativeImages::freed(DeviceHandle d,MemoryHandle m) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);if(auto* memory=impl_->memory(d,m)){
        impl_->retire(m,memory->generation,d);for(auto& image:impl_->images)if(image.device==d&&image.memory==m&&image.memoryGeneration==memory->generation){image.memory=0;image.memoryGeneration=0;}
        *memory={};}
}
bool NativeImages::exported(DeviceHandle d,MemoryHandle m,int fd,std::uint32_t type,std::uintptr_t caller) noexcept {
    if(type!=DmaBuf||!impl_->preparation||!impl_->preparation(caller,impl_->context))return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);auto* memory=impl_->memory(d,m);
    if(!impl_->healthy||!memory||!memory->dmaBuf)return false;
    Impl::Image* bound=nullptr;unsigned count=0;
    for(auto& image:impl_->images)if(image.device==d&&image.memory==m&&image.memoryGeneration==memory->generation){bound=&image;++count;}
    if(count!=1||!bound||!bound->description.dmaBuf||bound->offset!=0)return false;
    std::uint64_t device=0,inode=0;if(!identity(fd,device,inode))return false;
    const int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,0);if(duplicate<0)return false;
    std::uint64_t duplicateDevice=0,duplicateInode=0;
    if(!identity(duplicate,duplicateDevice,duplicateInode)||duplicateDevice!=device||duplicateInode!=inode){close(duplicate);return false;}
    // Repeated exports of the same allocation/image do not consume new slots.
    Impl::Export* empty=nullptr;
    for(auto& e:impl_->exports){
        if(e.owned<0){if(!empty)empty=&e;continue;}
        if(e.deviceIdentity==device&&e.inodeIdentity==inode){
            const bool same=e.device==d&&e.memory==m&&e.memoryGeneration==memory->generation&&e.imageGeneration==bound->generation;
            close(duplicate);if(!same)impl_->healthy=false;return same;}
    }
    if(!empty){close(duplicate);impl_->healthy=false;return false;}
    *empty={d,m,memory->generation,bound->generation,device,inode,duplicate};return true;
}
bool NativeImages::join(const CaptureObservation& capture,ImageLease& output) noexcept {
    if(!captureShape(capture))return false;
    std::uint64_t device=0,inode=0;if(!identity(capture.dmabufFd,device,inode))return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->healthy)return false;
    const Impl::Export* match=nullptr;unsigned count=0;
    for(const auto& e:impl_->exports)if(e.owned>=0&&e.deviceIdentity==device&&e.inodeIdentity==inode){match=&e;++count;}
    if(count!=1||!match)return false;
    auto* memory=impl_->memory(match->device,match->memory);if(!memory||memory->generation!=match->memoryGeneration)return false;
    const Impl::Image* image=nullptr;unsigned bindings=0;
    for(const auto& i:impl_->images)if(i.device==match->device&&i.memory==match->memory&&i.memoryGeneration==match->memoryGeneration){image=&i;++bindings;}
    if(bindings!=1||!image||image->generation!=match->imageGeneration||!image->description.dmaBuf||image->offset)return false;
    output={image->device,image->handle,image->memory,image->generation,image->memoryGeneration,device,inode,image->description,capture};return true;
}
bool NativeImages::current(const ImageLease& lease) const noexcept {
    if(!captureShape(lease.capture))return false;
    std::unique_lock<std::mutex> lock(impl_->mutex,std::try_to_lock);if(!lock||!impl_->healthy)return false;
    const auto* image=impl_->image(lease.device,lease.image);const auto* memory=impl_->memory(lease.device,lease.memory);
    if(!image||!memory||image->generation!=lease.imageGeneration||memory->generation!=lease.memoryGeneration
        ||image->memory!=lease.memory||image->memoryGeneration!=lease.memoryGeneration
        ||image->description.width!=lease.description.width||image->description.height!=lease.description.height
        ||image->description.format!=lease.description.format||image->description.dmaBuf!=lease.description.dmaBuf)return false;
    for(const auto& e:impl_->exports)if(e.owned>=0&&e.device==lease.device&&e.memory==lease.memory
        &&e.imageGeneration==lease.imageGeneration&&e.memoryGeneration==lease.memoryGeneration
        &&e.deviceIdentity==lease.deviceIdentity&&e.inodeIdentity==lease.inodeIdentity)return true;
    return false;
}
void NativeImages::reset() noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->reset();}
}
