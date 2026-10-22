// Actual process-entry worker starts before ordinary TU initialization, as the
// installed loader constructor does. Only pinned native decoder ports are fake.
#include "receiver/stream_bridge_native_runtime.h"
#include "receiver/stream_bridge_native_loader.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace nd=stream_bridge_native;
namespace {
std::atomic<unsigned> polls{0},restored{0};std::atomic<bool> imports{false};
void check(bool good,const char* text){if(!good){std::fprintf(stderr,"FAIL: %s\n",text);std::_Exit(1);}}
nd::RuntimeRequest request(){nd::RuntimeRequest r;r.explicitlyEnabled=true;r.buildId=712;r.pairingsDirectory="/unused-owned-fixture";r.producerPort=7;return r;}
void afterRuntime(){check(!nd::pinnedReceiverRunning()&&!imports.load(),"automatic exit joins the worker and restores imports before later teardown");std::puts("pre-main receiver lifetime and ordered exit passed");}
__attribute__((constructor(200))) void beforeOrdinaryInitialization(){
    check(std::atexit(afterRuntime)==0,"exit witness registered before actual process entry");
    check(nd::startPinnedReceiver(request()),"actual receiver starts before ordinary TU initialization");
}
}
namespace stream_bridge_native {
bool publishPinnedDecodeImports() noexcept{imports=true;return true;}
void restorePinnedDecodeImports() noexcept{if(imports.exchange(false))++restored;}
bool decodeImportsPublished() noexcept{return imports.load();}
bool pollPinnedProcess(Observation&,std::uint8_t*,std::size_t) noexcept{++polls;return false;}
bool pollPinnedCapture(CaptureObservation&) noexcept{return false;}
SnapshotStats pinnedSnapshotStats() noexcept{return {};}
bool pinnedCaptureLeaseCurrentOnWorker(const CaptureObservation&) noexcept{return false;}
bool setPinnedObservationWake(ObservationWake,void*) noexcept{return true;}
}
int main(){
    check(nd::pinnedReceiverRunning(),"ordinary initialization cannot overwrite a live pre-main receiver");
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(polls.load()<2&&std::chrono::steady_clock::now()<end)std::this_thread::yield();
    check(polls.load()>=2,"real background worker survives ordinary global initialization");
    for(unsigned n=0;n<16;++n){nd::stopPinnedReceiver();check(!nd::pinnedReceiverRunning()&&!imports.load(),"explicit stop joins before clearing worker ownership");check(nd::startPinnedReceiver(request())&&!nd::startPinnedReceiver(request()),"same process can restart once without a second owner");nd::wakePinnedReceiver();}
    check(restored.load()==16,"every explicit shutdown restores its own installed native imports once");
    // Deliberately leave a worker running. The real runtime's exit guard must
    // join it before destroying worker state; no test-owned cleanup does that.
    return 0;
}
