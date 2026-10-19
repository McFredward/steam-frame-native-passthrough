#include "pc/stream-bridge-sender-provider-coverage.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
using namespace stream_bridge::sender;
namespace {
unsigned checks=0;void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void run(){
    CoverageGateCounters counters;auto cold=counters.snapshot();
    check(!cold.maskedVideoBytes&&!cold.maskedWireBytes&&!cold.maskRleBytes&&!cold.maxMaskedWireBytes,"payload accounting starts empty without estimating network load");
    counters.payload(2000,500,2700);counters.payload(100,40,340);
    auto payload=counters.snapshot();check(payload.maskedVideoBytes==2100&&payload.maskRleBytes==540&&payload.maskedWireBytes==3040&&payload.maxMaskedWireBytes==2700,
        "paired-frame accounting separates original video, coverage and escaped/certified access units");
    counters.payload(std::numeric_limits<std::uint64_t>::max(),0,0);
    check(counters.snapshot().maskedVideoBytes==std::numeric_limits<std::uint64_t>::max(),"payload byte totals saturate without wrapping or touching frame authority");
    check(cold.hookDraws==0&&cold.nativeDraws==0&&cold.completePairs==0&&cold.lastRefusal==CoverageGate::None,"diagnostic counters start without rendering authority or samples");
    for(unsigned i=0;i<static_cast<unsigned>(CoverageSignal::Count);++i)
        for(unsigned n=0;n<=i;++n)counters.signal(static_cast<CoverageSignal>(i));
    auto snapshot=counters.snapshot();
    const std::uint64_t values[]{snapshot.hookDraws,snapshot.nativeDraws,snapshot.sourceFrames,snapshot.constantMaps,snapshot.constantCaptures,
        snapshot.eyeDraws,snapshot.completePairs,snapshot.nativeCopies,snapshot.capturedCopies,snapshot.matchedInputs,snapshot.certifiedMasks,
        snapshot.deviceOpened,snapshot.contextsReady,snapshot.otherDraws,snapshot.nativeMaps,snapshot.inventoryRefreshes};
    for(unsigned i=0;i<static_cast<unsigned>(CoverageSignal::Count);++i)check(values[i]==i+1,"every actual signal retains its own cold snapshot field");
    for(unsigned i=1;i<static_cast<unsigned>(CoverageGate::Count);++i){
        const auto gate=static_cast<CoverageGate>(i);check(!counters.refuse(gate),"recording a refusal cannot admit any GPU path");
        check(std::strcmp(coverageGateName(gate),"unknown")!=0,"every production refusal has a useful bounded static label");
    }
    snapshot=counters.snapshot();
    for(unsigned i=1;i<static_cast<unsigned>(CoverageGate::Count);++i)check(snapshot.refusals[i]==1,"cold snapshot preserves separate actual refusal populations");
    counters.signal(static_cast<CoverageSignal>(999));counters.refuse(static_cast<CoverageGate>(999));
    check(counters.snapshot().lastRefusal==snapshot.lastRefusal&&snapshot.refusals[0]==0,"invalid diagnostic selectors cannot index outside fixed storage or replace prior reason");
    counters.constantBytes(128);counters.source(2160,2160,28);counters.output(1152,4608,103);
    snapshot=counters.snapshot();check(snapshot.lastConstantBytes==128&&snapshot.lastSourceWidth==2160&&snapshot.lastSourceHeight==2160&&snapshot.lastSourceFormat==28
        &&snapshot.lastOutputWidth==1152&&snapshot.lastOutputHeight==4608&&snapshot.lastOutputFormat==103,"cold description copies only the already observed public D3D metadata");
    counters.submitted();counters.completion(0);counters.completion(1);counters.completion(0x887a0005u);
    snapshot=counters.snapshot();check(snapshot.submittedReadbacks==1&&snapshot.queryReady==1&&snapshot.queryPending==1&&snapshot.queryErrors==1
        &&snapshot.lastQueryResult==0x887a0005u,"completion pending is separate from Map busy and real query/device errors");
    counters.readback(0,125,82,19);counters.readback(0x887a000au,170,104,6);counters.readback(0x80070057u,90,35,2);
    snapshot=counters.snapshot();check(snapshot.readbackReady==1&&snapshot.readbackBusy==1&&snapshot.readbackErrors==1,
        "GPU pending is distinct from successful readback and API/device failures");
    check(snapshot.lastReadbackResult==0x80070057u&&snapshot.lastCopyAgeUs==90&&snapshot.maxCopyAgeUs==170
        &&snapshot.lastSubmissionAgeUs==35&&snapshot.lastReadbackUs==2&&snapshot.maxReadbackUs==19,
        "cold readback status retains native HRESULT and bounded stage lifetime measurements");
    std::atomic<std::uint64_t> full{std::numeric_limits<std::uint64_t>::max()-1};CoverageGateCounters::increment(full);
    check(full.load()==std::numeric_limits<std::uint64_t>::max(),"diagnostic counter can reach its bound without wrapping");CoverageGateCounters::increment(full);
    check(full.load()==std::numeric_limits<std::uint64_t>::max(),"bounded counter never wraps at saturation");
    CoverageGateCounters concurrent;std::thread threads[4];
    for(auto& thread:threads)thread=std::thread([&]{for(unsigned i=0;i<10000;++i){concurrent.signal(CoverageSignal::NativeDraws);concurrent.refuse(CoverageGate::ConstantBuffer);}});
    for(auto& thread:threads)thread.join();
    snapshot=concurrent.snapshot();
    check(snapshot.nativeDraws>0&&snapshot.nativeDraws<=40000&&snapshot.refusals[static_cast<unsigned>(CoverageGate::ConstantBuffer)]>0
        &&snapshot.refusals[static_cast<unsigned>(CoverageGate::ConstantBuffer)]<=40000,"contended one-attempt diagnostics remain bounded metadata, not coherent frame proof");
}
}
int main(){try{run();std::printf("sender native coverage gate diagnostics: %u checks PASS\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"coverage diagnostics failed after %u: %s\n",checks,e.what());return 1;}}
