#include "receiver/stream_bridge_native_images.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace nd=stream_bridge_native;
namespace {
unsigned checks=0;
void check(bool v,const char* name){++checks;if(!v)throw std::runtime_error(name);}
constexpr std::uintptr_t base=0x100000;
bool prep(std::uintptr_t caller,void*) noexcept{return caller==base+0x34574;}
struct File {int fd=-1;File(){char name[]="/tmp/sfnp-image712-XXXXXX";fd=mkstemp(name);check(fd>=0,"actual unique fd allocation fixture");unlink(name);check(ftruncate(fd,4096)==0,"actual fd allocation extent");}~File(){if(fd>=0)close(fd);}File(const File&)=delete;};
nd::ImageDescription desc(){return {2048,2048,1000156003,true};}
void resource(nd::NativeImages& images,std::uint64_t image=11,std::uint64_t memory=12){
    check(images.created(1,image,desc()),"actual public create success metadata");check(images.allocated(1,memory,true),"public external allocation success metadata");
    check(images.bound(1,image,memory,0),"public image-memory binding retained");}
nd::CaptureObservation capture(int fd){return {nd::CaptureKind::Decoded,23,fd,2,0x4321,7,21,9};}
void publicJoin(){
    File file;nd::NativeImages images(prep,nullptr);resource(images);
    check(!images.exported(1,12,file.fd,0x200,base+0x34578),"unknown native export preparation callsite refused");
    check(!images.exported(1,12,file.fd,1,base+0x34574),"OPAQUE_FD cannot masquerade as DMA_BUF");
    check(images.exported(1,12,file.fd,0x200,base+0x34574),"cold actual fd duplicated while still owned at public export");
    nd::ImageLease lease{};check(images.join(capture(file.fd),lease),"exact successful CAPTURE fd joins public exported memory and image");
    check(lease.device==1&&lease.image==11&&lease.memory==12&&lease.imageGeneration&&lease.memoryGeneration
        &&lease.capture.contentGeneration==9&&lease.capture.nativeTimestampToken==0x4321&&lease.description.width==2048
        &&lease.deviceIdentity&&lease.inodeIdentity,"lease keeps source capture fields distinct from public image allocation generations");
    check(images.current(lease),"known live image allocation passes scalar nonblocking current check");
    const int alias=fcntl(file.fd,F_DUPFD_CLOEXEC,0);check(alias>=0,"fixture duplicate actual allocation fd");
    nd::ImageLease same{};check(images.join(capture(alias),same)&&same.image==11,"actual fd aliases join by dev+inode instead of raw fd number");close(alias);
    for(unsigned i=0;i<100;++i)check(images.exported(1,12,file.fd,0x200,base+0x34574),"repeat export same allocation is bounded/idempotent");
    images.destroyed(1,11);check(!images.current(lease)&&!images.join(capture(file.fd),same),"actual image destroy revokes allocation and export immediately");
    check(images.created(1,11,desc())&&images.bound(1,11,12,0)&&images.exported(1,12,file.fd,0x200,base+0x34574),"same public image handle may be reused only as distinct allocation generation");
    check(images.join(capture(file.fd),same)&&same.imageGeneration!=lease.imageGeneration&&!images.current(lease),"recreated same handles cannot revive old image generation");
    images.freed(1,12);check(!images.current(same)&&!images.join(capture(file.fd),same),"freeMemory retires owned duplicate and all bound identities");
}
void fdReuseAndAmbiguity(){
    File old,newer;nd::NativeImages images(prep,nullptr);resource(images);check(images.exported(1,12,old.fd,0x200,base+0x34574),"fixture old memory exported");
    const int raw=old.fd;check(dup2(newer.fd,raw)==raw,"actual OS reuses old exported fd number for another allocation");
    nd::ImageLease untouched{};untouched.image=99;
    check(!images.join(capture(raw),untouched)&&untouched.image==99,"same raw fd with different actual allocation cannot select old image; output transactional");
    images.reset();resource(images);check(images.exported(1,12,raw,0x200,base+0x34574),"new allocation can be explicitly exported after reset");
    resource(images,21,22);check(!images.exported(1,22,raw,0x200,base+0x34574)&&!images.join(capture(raw),untouched),"one real allocation aliased by unrelated public image exports invalidates whole ambiguous registry");
    images.reset();resource(images);check(images.exported(1,12,raw,0x200,base+0x34574),"single image fixture");
    check(images.created(1,21,desc())&&images.bound(1,21,12,0),"public shared-memory second image observed");
    check(!images.join(capture(raw),untouched),"memory with multiple public images has no unique selected-image mapping");
}
void refusesAndCapacity(){
    File file;nd::NativeImages defaultImages;resource(defaultImages);
    check(!defaultImages.exported(1,12,file.fd,0x200,base+0x34574),"unrelated actual executable cannot satisfy production export source gate");
    nd::NativeImages images(prep,nullptr);resource(images);
    check(!images.exported(1,12,-1,0x200,base+0x34574),"closed/invalid fd gives no export");
    auto off=desc();off.dmaBuf=false;check(!images.created(1,22,off)&&images.allocated(1,23,true)&&!images.bound(1,22,23,0),"ordinary images do not consume video registry slots");
    check(!images.exported(1,23,file.fd,0x200,base+0x34574),"non-external public image cannot become exported video");
    check(images.created(1,24,desc())&&!images.allocated(1,25,false)&&!images.bound(1,24,25,0),"ordinary memories do not consume video registry slots");
    check(!images.exported(1,25,file.fd,0x200,base+0x34574),"non-export allocation cannot become video");
    check(images.created(1,26,desc())&&images.allocated(1,27,true)&&images.bound(1,26,27,4096),"offset binding known");
    check(!images.exported(1,27,file.fd,0x200,base+0x34574),"unsupported nonzero binding offset refused");
    check(images.exported(1,12,file.fd,0x200,base+0x34574),"valid owned fd fixture");nd::ImageLease lease{};
    for(unsigned bad=0;bad<7;++bad){auto c=capture(file.fd);if(bad==0)c.kind=nd::CaptureKind::Requeue;if(bad==1)c.capturePoolGeneration=0;
        if(bad==2){c.contentGeneration=0;}if(bad==3){c.sourcePoolGeneration=0;}if(bad==4){c.nativeTimestampToken=0x10000;}
        if(bad==5){c.index=16;}if(bad==6){c.dmabufFd=-1;}check(!images.join(c,lease),"unproved/unsupported capture shape cannot join image");}
    nd::NativeImages full(prep,nullptr);for(unsigned i=1;i<=nd::NativeImages::Capacity;++i)check(full.created(1,i,desc()),"fixed image slot");
    check(!full.created(1,1000,desc()),"image registry does not grow beyond fixed capacity");
    full.reset();check(full.created(1,1,desc()),"explicit resource reset permits fresh bounded source");
}
}
int main(){try{publicJoin();fdReuseAndAmbiguity();refusesAndCapacity();std::printf("public Vulkan image allocation join: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
