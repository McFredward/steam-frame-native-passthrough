#pragma once
#include "stream-bridge-native-image.h"
#include "stream-bridge-sender-provider-directmode.h"
#include "stream-bridge-sender-provider-core.h"
#include "stream-bridge-source-contract.h"
#include "stream-bridge-source-alpha-stats.h"
#include "stream-bridge-mask-content.h"
#include "bridge/stream_bridge_control.h"
#include <memory>
#include <limits>
struct ID3D11DeviceContext;
namespace stream_bridge::sender {
// Diagnostic metadata only, never a permission or a GPU-frame identity. Fixed
// storage and one non-retrying CAS per sample keep render callbacks bounded.
enum class CoverageGate : unsigned {
    None,Device,Context,Caller,DirectFrame,Lock,Prepared,DrawShape,FrameEpoch,
    ConstantBuffer,EyeSelector,SourceView,SourceShape,SharedResource,AmbiguousSource,
    RepeatedSource,MixedProcess,InteriorCopy,OutputDepth,OutputShape,BlendDraw,
    CopyPair,StageFull,InputReceipt,RawWitness,Readback,Inventory,Encoder,SourceContract,CoverageSession,StageReceipt,RleEncode,FrameCertificate,SourceRevoked,QueryPending,QueryError,Count
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
    case CoverageGate::Inventory:return "source-inventory";case CoverageGate::Encoder:return "native-encoder";
    case CoverageGate::SourceContract:return "source-format-contract";
    case CoverageGate::CoverageSession:return "coverage-session";case CoverageGate::StageReceipt:return "staging-receipt";
    case CoverageGate::RleEncode:return "coverage-rle";case CoverageGate::FrameCertificate:return "frame-certificate";case CoverageGate::SourceRevoked:return "source-format-revoked";case CoverageGate::QueryPending:return "readback-query-pending";case CoverageGate::QueryError:return "readback-query-error";default:return "unknown";}
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
    std::uint32_t contextHookSlots=0,contextOriginalSlots=0,contextFacadeSlots=0,nativeDispatchMutations=0;
    std::uint64_t readbackReady=0,readbackBusy=0,readbackErrors=0,
        lastCopyAgeUs=0,maxCopyAgeUs=0,lastSubmissionAgeUs=0,lastReadbackUs=0,maxReadbackUs=0;
    std::uint64_t submittedReadbacks=0,queryReady=0,queryPending=0,queryErrors=0,lastQueryAgeUs=0,maxQueryAgeUs=0;
    std::uint32_t lastQueryResult=0,lastReadbackResult=0;
    std::uint64_t maskedVideoBytes=0,maskedWireBytes=0,maskRleBytes=0,maxMaskedWireBytes=0;
    std::uint64_t maskEncodedBytes=0,maskRawBytes=0,lastEncodeUs=0,maxEncodeUs=0;
    std::array<std::uint64_t,6> maskEncodingFrames{};
    std::array<std::uint64_t,static_cast<unsigned>(SourceContractStatus::Count)> sourceRefusals{},sourceProcessPairs{};
    std::uint32_t sourceCauses=0;
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
    void sourceRefusal(const SourceContractObservation& observed)noexcept{
        const auto at=static_cast<unsigned>(observed.status);
        if(!at||at>=sourceRefusals_.size())return;
        increment(sourceRefusals_[at]);
        sourceProcessPairs_[at].store((std::uint64_t(observed.declaredProcess)<<32)|observed.nativeProcess,std::memory_order_relaxed);
        // A first cause must survive simultaneous approximate counter samples.
        // This bitset is diagnostic only and can never grant source permission.
        sourceCauses_.fetch_or(std::uint32_t(1)<<at,std::memory_order_release);
    }
    void constantBytes(std::uint32_t bytes)noexcept{constant_.store(bytes,std::memory_order_relaxed);}
    void source(std::uint32_t width,std::uint32_t height,std::uint32_t format)noexcept{sourceWidth_.store(width,std::memory_order_relaxed);sourceHeight_.store(height,std::memory_order_relaxed);sourceFormat_.store(format,std::memory_order_relaxed);}
    void output(std::uint32_t width,std::uint32_t height,std::uint32_t format)noexcept{outputWidth_.store(width,std::memory_order_relaxed);outputHeight_.store(height,std::memory_order_relaxed);outputFormat_.store(format,std::memory_order_relaxed);}
    void submitted()noexcept{increment(submittedReadbacks_);}
    void payload(std::uint64_t video,std::uint64_t encoded,std::uint64_t wire,
                 unsigned encoding=2,std::uint64_t raw=0,std::uint64_t encodeUs=0)noexcept{
        add(maskedVideoBytes_,video);add(maskEncodedBytes_,encoded);add(maskRawBytes_,raw);
        // Preserve the existing metric's meaning: compressed and raw masks are
        // counted separately, never relabelled as ordinary RLE bytes.
        if(encoding==2)add(maskRleBytes_,encoded);
        if(encoding<maskEncodingFrames_.size())increment(maskEncodingFrames_[encoding]);
        encodeDuration_.store(encodeUs,std::memory_order_relaxed);maximum(maxEncodeDuration_,encodeUs);
        add(maskedWireBytes_,wire);maximum(maxMaskedWireBytes_,wire);
    }
    void completion(std::uint32_t result,std::uint64_t ageUs=0)noexcept{
        increment(result==0?queryReady_:(result==1?queryPending_:queryErrors_));
        queryResult_.store(result,std::memory_order_relaxed);queryAge_.store(ageUs,std::memory_order_relaxed);maximum(maxQueryAge_,ageUs);
    }
    void readback(std::uint32_t result,std::uint64_t copyAgeUs,std::uint64_t submissionAgeUs,std::uint64_t durationUs)noexcept{
        // Public DXGI_ERROR_WAS_STILL_DRAWING; this header is also built on Linux.
        increment(!(result&0x80000000u)?readbackReady_:(result==0x887a000au?readbackBusy_:readbackErrors_));
        readbackResult_.store(result,std::memory_order_relaxed);
        copyAge_.store(copyAgeUs,std::memory_order_relaxed);submissionAge_.store(submissionAgeUs,std::memory_order_relaxed);
        readbackDuration_.store(durationUs,std::memory_order_relaxed);
        maximum(maxCopyAge_,copyAgeUs);maximum(maxReadbackDuration_,durationUs);
    }
    CoverageGateStats snapshot()const noexcept{
        CoverageGateStats out;std::uint64_t* const values[]{&out.hookDraws,&out.nativeDraws,&out.sourceFrames,&out.constantMaps,&out.constantCaptures,
            &out.eyeDraws,&out.completePairs,&out.nativeCopies,&out.capturedCopies,&out.matchedInputs,&out.certifiedMasks,
            &out.deviceOpened,&out.contextsReady,&out.otherDraws,&out.nativeMaps,&out.inventoryRefreshes};
        static_assert(sizeof values/sizeof values[0]==static_cast<unsigned>(CoverageSignal::Count),"all diagnostic signals are exposed");
        for(unsigned i=0;i<signals_.size();++i)*values[i]=signals_[i].load(std::memory_order_relaxed);
        for(unsigned i=0;i<refusals_.size();++i)out.refusals[i]=refusals_[i].load(std::memory_order_relaxed);
        out.sourceCauses=sourceCauses_.load(std::memory_order_acquire);
        for(unsigned i=0;i<sourceRefusals_.size();++i){out.sourceRefusals[i]=sourceRefusals_[i].load(std::memory_order_relaxed);out.sourceProcessPairs[i]=sourceProcessPairs_[i].load(std::memory_order_relaxed);}
        out.lastRefusal=static_cast<CoverageGate>(last_.load(std::memory_order_relaxed));out.lastConstantBytes=constant_.load(std::memory_order_relaxed);
        out.lastSourceWidth=sourceWidth_.load(std::memory_order_relaxed);out.lastSourceHeight=sourceHeight_.load(std::memory_order_relaxed);out.lastSourceFormat=sourceFormat_.load(std::memory_order_relaxed);
        out.lastOutputWidth=outputWidth_.load(std::memory_order_relaxed);out.lastOutputHeight=outputHeight_.load(std::memory_order_relaxed);out.lastOutputFormat=outputFormat_.load(std::memory_order_relaxed);out.readbackReady=readbackReady_.load(std::memory_order_relaxed);out.readbackBusy=readbackBusy_.load(std::memory_order_relaxed);out.readbackErrors=readbackErrors_.load(std::memory_order_relaxed);
        out.submittedReadbacks=submittedReadbacks_.load(std::memory_order_relaxed);out.queryReady=queryReady_.load(std::memory_order_relaxed);out.queryPending=queryPending_.load(std::memory_order_relaxed);out.queryErrors=queryErrors_.load(std::memory_order_relaxed);out.lastQueryResult=queryResult_.load(std::memory_order_relaxed);out.lastQueryAgeUs=queryAge_.load(std::memory_order_relaxed);out.maxQueryAgeUs=maxQueryAge_.load(std::memory_order_relaxed);
        out.lastReadbackResult=readbackResult_.load(std::memory_order_relaxed);out.lastCopyAgeUs=copyAge_.load(std::memory_order_relaxed);out.maxCopyAgeUs=maxCopyAge_.load(std::memory_order_relaxed);
        out.lastSubmissionAgeUs=submissionAge_.load(std::memory_order_relaxed);out.lastReadbackUs=readbackDuration_.load(std::memory_order_relaxed);out.maxReadbackUs=maxReadbackDuration_.load(std::memory_order_relaxed);
        out.maskedVideoBytes=maskedVideoBytes_.load(std::memory_order_relaxed);out.maskRleBytes=maskRleBytes_.load(std::memory_order_relaxed);out.maskedWireBytes=maskedWireBytes_.load(std::memory_order_relaxed);out.maxMaskedWireBytes=maxMaskedWireBytes_.load(std::memory_order_relaxed);
        out.maskEncodedBytes=maskEncodedBytes_.load(std::memory_order_relaxed);out.maskRawBytes=maskRawBytes_.load(std::memory_order_relaxed);
        out.lastEncodeUs=encodeDuration_.load(std::memory_order_relaxed);out.maxEncodeUs=maxEncodeDuration_.load(std::memory_order_relaxed);
        for(unsigned i=0;i<maskEncodingFrames_.size();++i)out.maskEncodingFrames[i]=maskEncodingFrames_[i].load(std::memory_order_relaxed);
        return out;
    }
private:
    static void add(std::atomic<std::uint64_t>& value,std::uint64_t sample)noexcept{
        auto current=value.load(std::memory_order_relaxed);const auto remaining=std::numeric_limits<std::uint64_t>::max()-current;
        value.compare_exchange_strong(current,current+(sample<remaining?sample:remaining),std::memory_order_relaxed,std::memory_order_relaxed);
    }
    static void maximum(std::atomic<std::uint64_t>& value,std::uint64_t sample)noexcept{
        auto current=value.load(std::memory_order_relaxed);if(sample>current)value.compare_exchange_strong(current,sample,std::memory_order_relaxed,std::memory_order_relaxed);
    }
    std::atomic<std::uint64_t> readbackReady_{},readbackBusy_{},readbackErrors_{},copyAge_{},maxCopyAge_{},submissionAge_{},readbackDuration_{},maxReadbackDuration_{};
    std::atomic<std::uint32_t> readbackResult_{},queryResult_{};
    std::atomic<std::uint64_t> submittedReadbacks_{},queryReady_{},queryPending_{},queryErrors_{},queryAge_{},maxQueryAge_{};
    std::atomic<std::uint64_t> maskedVideoBytes_{},maskedWireBytes_{},maskRleBytes_{},maxMaskedWireBytes_{};
    std::atomic<std::uint64_t> maskEncodedBytes_{},maskRawBytes_{},encodeDuration_{},maxEncodeDuration_{};
    std::array<std::atomic<std::uint64_t>,6> maskEncodingFrames_{};
    // Contention can lose diagnostic samples; snapshots are intentionally not
    // coherent frame proofs. No caller may use them to authorize rendering.
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(CoverageSignal::Count)> signals_{};
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(CoverageGate::Count)> refusals_{};
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(SourceContractStatus::Count)> sourceRefusals_{},sourceProcessPairs_{};
    std::atomic<std::uint32_t> sourceCauses_{0};
    std::atomic<unsigned> last_{0};std::atomic<std::uint32_t> constant_{0},sourceWidth_{0},sourceHeight_{0},sourceFormat_{0},outputWidth_{0},outputHeight_{0},outputFormat_{0};
};
// Cold caller already enforces its five-second cadence. Keep first observations
// of every source cause, plus at most eight changing-counter snapshots, separate
// from the much larger generic coverage report's exhausted sampling budget.
class CoverageSourceReportBudget {
public:
    bool observe(const CoverageGateStats& stats,bool final=false)noexcept{
        if(!stats.sourceCauses)return false;
        const bool newCause=(stats.sourceCauses&~seen_)!=0;
        const bool changed=stats.sourceRefusals!=reported_;
        if(!final&&!newCause&&(!changed||changes_>=8))return false;
        if(!newCause&&!final)++changes_;
        seen_|=stats.sourceCauses;reported_=stats.sourceRefusals;return true;
    }
private:
    std::uint32_t seen_=0;unsigned changes_=0;
    std::array<std::uint64_t,static_cast<unsigned>(SourceContractStatus::Count)> reported_{};
};
// A writable, object-local facade. Every public method forwards through the
// original runtime table at call time; no system-code/table entry is changed.
// Callbacks replace Draw, Map, Unmap and CopyResource only, in that order.
class CoverageContextFacade {
public:
    CoverageContextFacade();
    ~CoverageContextFacade();
    CoverageContextFacade(const CoverageContextFacade&)=delete;
    CoverageContextFacade& operator=(const CoverageContextFacade&)=delete;
    bool prepare(ID3D11DeviceContext*,const std::array<std::uintptr_t,4>& callbacks)noexcept;
    bool publish()noexcept;
    bool restore()noexcept;
    bool installed()const noexcept;
    unsigned slotCount()const noexcept;
    std::uintptr_t originalFunction(unsigned slot)const noexcept;
    std::uint32_t nativeMutationCount()const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
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
    bool configure(std::uintptr_t verifiedNativeModule,const NativeImageLayout&)noexcept;
    void tick()noexcept;
    void sourceProcess(std::uint32_t processId,bool metadataBudget=false)noexcept; // Cold live-app format lease; zero revokes.
    void stop()noexcept;
    bool ready(std::uint32_t processId)const noexcept;
    bool operational(std::uint32_t processId)const noexcept;
    CoverageGateStats gateStats()const noexcept; // Cold, approximate diagnostics only.
    bool sourceAlpha(unsigned eye,SourceAlphaSample&)const noexcept;
    bool maskContent(MaskContentSample&)const noexcept;
    std::uint32_t sourceAlphaError()const noexcept;
    const char* stateName()const noexcept; // Bounded cold-worker status, no identifiers/secrets.
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace stream_bridge::sender
