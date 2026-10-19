#pragma once
#include <openvr_driver.h>
#include "stream-bridge-sender-provider-swap-shape.h"
#include <array>
#include <atomic>
#include <mutex>
namespace stream_bridge::sender {
using DirectMode=vr::IVRDriverDirectModeComponent;
inline bool decodeSwapTextureSetDesc(const DirectMode::SwapTextureSetDesc_t& d,
    SharedTextureShape& shape)noexcept{
    return decodePinnedSwapShape(d.nWidth,d.nHeight,d.nFormat,d.nSampleCount,shape);
}
struct DirectEye {
    DirectMode::SubmitLayerPerEye_t submitted{};
    DirectMode::SwapTextureSetDesc_t description{};
    std::uint64_t swapGeneration=0;std::uint32_t processId=0;
};
struct DirectFrame {
    void* component=nullptr;std::uint64_t presentSerial=0,registryEpoch=0;
    std::array<std::array<DirectEye,2>,4> layers{};std::uint32_t layerCount=0;
    bool sourceKnown=false;
};
struct DirectSwap {void* component=nullptr;std::uint32_t processId=0;std::uint64_t generation=0;DirectMode::SwapTextureSetDesc_t description{};std::array<vr::SharedTextureHandle_t,3> handles{};};
struct DirectInventory {std::array<DirectSwap,64> swaps{};std::uint64_t epoch=0;bool valid=false;};
struct DirectModeFunctions {
    void (*create)(void*,std::uint32_t,const DirectMode::SwapTextureSetDesc_t*,DirectMode::SwapTextureSet_t*)=nullptr;
    void (*destroy)(void*,vr::SharedTextureHandle_t)=nullptr;
    void (*destroyAll)(void*,std::uint32_t)=nullptr;
    void (*next)(void*,vr::SharedTextureHandle_t[2],std::uint32_t(*)[2])=nullptr;
    void (*submit)(void*,const DirectMode::SubmitLayerPerEye_t(&)[2])=nullptr;
    void (*present)(void*,vr::SharedTextureHandle_t)=nullptr;
    void (*post)(void*,const DirectMode::Throttling_t*)=nullptr;
    void (*timing)(void*,vr::DriverDirectMode_FrameTiming*)=nullptr;
};
// Public DirectMode009 calls only. Native installer verifies all eight concrete
// original pointers before publication. Metadata never acquires GPU resources or
// proves alpha/mapping; the actual native composition must supply those facts.
class DirectModeBoundary {
public:
    bool prepare(const DirectModeFunctions&) noexcept;
    DirectModeFunctions entryPoints()const noexcept;
    void stop()noexcept;
    bool ownsProcess(std::uint32_t)const noexcept;
    // Cold change detection only; never certifies a texture/frame. An unchanged
    // registry needs no repeated lock/copy that can contend with native Submit.
    std::uint64_t inventoryRevision()const noexcept;
    DirectInventory inventory()const noexcept;
    static const DirectFrame* currentFrame()noexcept;
private:
    struct Swap{void* component=nullptr;std::uint32_t pid=0;std::uint64_t generation=0;DirectMode::SwapTextureSetDesc_t description{};std::array<vr::SharedTextureHandle_t,3> handles{};};
    struct Component{void* value=nullptr;DirectFrame queued{},presented{};};
    static void create(void*,std::uint32_t,const DirectMode::SwapTextureSetDesc_t*,DirectMode::SwapTextureSet_t*);
    static void destroy(void*,vr::SharedTextureHandle_t);
    static void destroyAll(void*,std::uint32_t);
    static void next(void*,vr::SharedTextureHandle_t[2],std::uint32_t(*)[2]);
    static void submit(void*,const DirectMode::SubmitLayerPerEye_t(&)[2]);
    static void present(void*,vr::SharedTextureHandle_t);
    static void post(void*,const DirectMode::Throttling_t*);
    static void timing(void*,vr::DriverDirectMode_FrameTiming*);
    Component* component(void*);
    DirectModeFunctions original_{};mutable std::mutex mutex_;
    std::array<Swap,64> swaps_{};std::array<Component,4> components_{};
    std::uint64_t nextSwap_=0,nextPresent_=0;bool prepared_=false;
    std::atomic<std::uint64_t> registryEpoch_{1};std::atomic<bool> enabled_{false},tainted_{false};
    static std::atomic<DirectModeBoundary*> instance_;
    static thread_local const DirectFrame* current_;
};
} // namespace stream_bridge::sender
