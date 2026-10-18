#pragma once
#include "stream-bridge-sender-provider-directmode.h"
#include "stream-bridge-sender-provider-core.h"
#include "bridge/stream_bridge_control.h"
#include <memory>
#include <limits>
namespace stream_bridge::sender {
// Diagnostic metadata only, never a permission or a GPU-frame identity. Fixed
// storage and one non-retrying CAS per sample keep render callbacks bounded.
enum class CoverageGate : unsigned {
    None,Device,Context,Caller,DirectFrame,Lock,Prepared,DrawShape,FrameEpoch,
    ConstantBuffer,EyeSelector,SourceView,SourceShape,SharedResource,AmbiguousSource,
    RepeatedSource,MixedProcess,InteriorCopy,OutputDepth,OutputShape,BlendDraw,
    CopyPair,StageFull,InputReceipt,RawWitness,Readback,Inventory,Encoder,Count
};
inline const char* coverageGateName(CoverageGate gate)noexcept{
    switch(gate){
    case CoverageGate::None:return "none";case CoverageGate::Device:return "native-device";
    case CoverageGate::Context:return "native-context";case CoverageGate::Caller:return "native-caller";
    case CoverageGate::DirectFrame:return "direct-frame";case CoverageGate::Lock:return "metadata-lock";
    case CoverageGate::Prepared:return "pool-prepared";case CoverageGate::DrawShape:return "draw-shape";
    case CoverageGate::FrameEpoch:return "frame-epoch";case CoverageGate::ConstantBuffer:return "constant-buffer";
    case CoverageGate::EyeSelector:return "eye-selector";case CoverageGate::SourceView:return "source-view";
    case CoverageGate::SourceShape:return "source-shape";case CoverageGate::SharedResource:return "shared-source";
    case CoverageGate::AmbiguousSource:return "ambiguous-source";case CoverageGate::RepeatedSource:return "repeated-eye";
    case CoverageGate::MixedProcess:return "mixed-process";case CoverageGate::InteriorCopy:return "source-interior-copy";
    case CoverageGate::OutputDepth:return "output-depth";case CoverageGate::OutputShape:return "output-shape";
    case CoverageGate::BlendDraw:return "coverage-draw";case CoverageGate::CopyPair:return "copy-pair";
    case CoverageGate::StageFull:return "staging-capacity";case CoverageGate::InputReceipt:return "input-receipt";
    case CoverageGate::RawWitness:return "source-alpha-witness";case CoverageGate::Readback:return "readback";
    case CoverageGate::Inventory:return "source-inventory";case CoverageGate::Encoder:return "native-encoder";default:return "unknown";}
}
enum class CoverageSignal : unsigned {
    HookDraws,NativeDraws,SourceFrames,ConstantMaps,ConstantCaptures,EyeDraws,
    CompletePairs,NativeCopies,CapturedCopies,MatchedInputs,CertifiedMasks,
    DeviceOpened,ContextsReady,OtherDraws,NativeMaps,InventoryRefreshes,Count
};
struct CoverageGateStats {
    std::uint64_t hookDraws=0,nativeDraws=0,sourceFrames=0,constantMaps=0,constantCaptures=0,
        eyeDraws=0,completePairs=0,nativeCopies=0,capturedCopies=0,matchedInputs=0,certifiedMasks=0,
        deviceOpened=0,contextsReady=0,otherDraws=0,nativeMaps=0,inventoryRefreshes=0;
    CoverageGate lastRefusal=CoverageGate::None;
    std::array<std::uint64_t,static_cast<unsigned>(CoverageGate::Count)> refusals{};
    std::uint32_t lastConstantBytes=0,lastSourceWidth=0,lastSourceHeight=0,lastSourceFormat=0,
        lastOutputWidth=0,lastOutputHeight=0,lastOutputFormat=0;
    // Cold dispatch-integrity metadata. Bits 0..3 name Draw, Map, Unmap,
    // CopyResource on the retained context's currently published vtable.
    // Approximate diagnostics only; these fields never authorize a frame.
    std::uint32_t contextHookSlots=0;
    bool contextTableReadable=false,contextTableStable=false,nativeDeviceSame=false;
};
class CoverageGateCounters {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"render diagnostics must never use an atomic-library lock");
public:
    static void increment(std::atomic<std::uint64_t>& value)noexcept{
        auto current=value.load(std::memory_order_relaxed);
        if(current!=std::numeric_limits<std::uint64_t>::max())
            value.compare_exchange_strong(current,current+1,std::memory_order_relaxed,std::memory_order_relaxed);
    }
    void signal(CoverageSignal signal)noexcept{const auto at=static_cast<unsigned>(signal);if(at<signals_.size())increment(signals_[at]);}
    bool refuse(CoverageGate gate)noexcept{
        const auto at=static_cast<unsigned>(gate);if(at&&at<refusals_.size()){increment(refusals_[at]);last_.store(at,std::memory_order_relaxed);}return false;
    }
    void constantBytes(std::uint32_t bytes)noexcept{constant_.store(bytes,std::memory_order_relaxed);}
    void source(std::uint32_t width,std::uint32_t height,std::uint32_t format)noexcept{sourceWidth_.store(width,std::memory_order_relaxed);sourceHeight_.store(height,std::memory_order_relaxed);sourceFormat_.store(format,std::memory_order_relaxed);}
    void output(std::uint32_t width,std::uint32_t height,std::uint32_t format)noexcept{outputWidth_.store(width,std::memory_order_relaxed);outputHeight_.store(height,std::memory_order_relaxed);outputFormat_.store(format,std::memory_order_relaxed);}
    CoverageGateStats snapshot()const noexcept{
        CoverageGateStats out;std::uint64_t* const values[]{&out.hookDraws,&out.nativeDraws,&out.sourceFrames,&out.constantMaps,&out.constantCaptures,
            &out.eyeDraws,&out.completePairs,&out.nativeCopies,&out.capturedCopies,&out.matchedInputs,&out.certifiedMasks,
            &out.deviceOpened,&out.contextsReady,&out.otherDraws,&out.nativeMaps,&out.inventoryRefreshes};
        static_assert(sizeof values/sizeof values[0]==static_cast<unsigned>(CoverageSignal::Count),"all diagnostic signals are exposed");
        for(unsigned i=0;i<signals_.size();++i)*values[i]=signals_[i].load(std::memory_order_relaxed);
        for(unsigned i=0;i<refusals_.size();++i)out.refusals[i]=refusals_[i].load(std::memory_order_relaxed);
        out.lastRefusal=static_cast<CoverageGate>(last_.load(std::memory_order_relaxed));out.lastConstantBytes=constant_.load(std::memory_order_relaxed);
        out.lastSourceWidth=sourceWidth_.load(std::memory_order_relaxed);out.lastSourceHeight=sourceHeight_.load(std::memory_order_relaxed);out.lastSourceFormat=sourceFormat_.load(std::memory_order_relaxed);
        out.lastOutputWidth=outputWidth_.load(std::memory_order_relaxed);out.lastOutputHeight=outputHeight_.load(std::memory_order_relaxed);out.lastOutputFormat=outputFormat_.load(std::memory_order_relaxed);return out;
    }
private:
    // Contention can lose diagnostic samples; snapshots are intentionally not
    // coherent frame proofs. No caller may use them to authorize rendering.
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(CoverageSignal::Count)> signals_{};
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(CoverageGate::Count)> refusals_{};
    std::atomic<unsigned> last_{0};std::atomic<std::uint32_t> constant_{0},sourceWidth_{0},sourceHeight_{0},sourceFormat_{0},outputWidth_{0},outputHeight_{0},outputFormat_{0};
};
// Process-lifetime native GPU observer. Construct before publishing the NVENC
// factory; tick runs only on the cold provider worker. All refusals leave native
// video alone. ready is source-mode preparation, never selected-image proof.
class SenderCoverage {
public:
    SenderCoverage(DirectModeBoundary&,NvencBoundary&,ControlRuntime&);
    ~SenderCoverage();
    SenderCoverage(const SenderCoverage&)=delete;
    SenderCoverage& operator=(const SenderCoverage&)=delete;
    bool configure(std::uintptr_t verifiedNativeModule)noexcept;
    void tick()noexcept;
    void stop()noexcept;
    bool ready(std::uint32_t processId)const noexcept;
    bool operational(std::uint32_t processId)const noexcept;
    CoverageGateStats gateStats()const noexcept; // Cold, approximate diagnostics only.
    const char* stateName()const noexcept; // Bounded cold-worker status, no identifiers/secrets.
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge::sender
