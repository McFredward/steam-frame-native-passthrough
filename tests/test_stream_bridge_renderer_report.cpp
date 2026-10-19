#include "layer/stream_bridge_report_policy.h"
#include <cstdio>
#include <stdexcept>

using namespace stream_bridge_renderer;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Fixture {
    ColdReportPolicy policy;RendererStats state,output;unsigned samples=0;
    bool tick(std::int64_t milliseconds){
        return policy.inspect(ColdReportPolicy::Clock::time_point{}+std::chrono::milliseconds(milliseconds),
            [&]{++samples;return state;},output);
    }
};
void samplingAndStartup(){
    Fixture f;
    check(!f.tick(0),"empty startup does not log");
    f.state.metadata=1;
    for(int t=1;t<5000;++t)check(!f.tick(t),"startup ticks remain sampled at most once per five seconds");
    check(f.samples==1,"throttle precedes expensive sampling even before first metadata");
    check(f.tick(5000),"first active snapshot appears at next bounded sample");
    check(f.output.metadata==1,"published snapshot contains the real sampled counters");
    for(int t=5001;t<10000;++t){++f.state.maskRejected;check(!f.tick(t),"even startup anomalies cannot form a burst");}
    check(f.samples==2,"frequent error changes do not invoke pool sampling");
    check(f.tick(10000),"startup error growth is reported after five seconds");
}
void healthyLifetime(){
    Fixture f;f.state.metadata=4001;f.state.maskDecoded=4000;f.state.maskNone=1;
    check(f.tick(0),"initial active snapshot");
    unsigned reports=1;
    // Accelerated four-hour cold-worker lifetime, beyond the former 192 cap.
    for(std::int64_t t=1;t<=14400000;t+=2){
        ++f.state.metadata;++f.state.maskNone;++f.state.opaqueSubmits;
        ++f.state.drawRefusals[static_cast<std::size_t>(DrawRefusal::Pipeline)];
        ++f.state.drawRefusals[static_cast<std::size_t>(DrawRefusal::Camera)];
        ++f.state.collectRefusals[static_cast<std::size_t>(CollectRefusal::Empty)];
        ++f.state.collectRefusals[static_cast<std::size_t>(CollectRefusal::Camera)];
        if(f.tick(t))++reports;
    }
    check(reports==480,"healthy None/native work reports only every thirty seconds throughout four hours");
    check(f.samples==2880,"two-millisecond worker ticks sample the pool only every five seconds");
    check(f.tick(14400001),"last interval progress is published after four hours");
    const auto before=f.samples;
    check(!f.tick(14430001),"unchanged healthy phase does not report merely because time passed");
    check(f.samples==before+1,"idle check is still a bounded sample");
    ++f.state.drawRefusals[static_cast<std::size_t>(DrawRefusal::ExactPicture)];
    check(f.tick(14435001),"new exact-picture failure still reports after four hours");
}
void anomaliesAndIdle(){
    Fixture f;f.state.metadata=1;f.state.maskDecoded=1;
    check(f.tick(0),"first mask snapshot");
    ++f.state.maskDecoded;++f.state.metadata;
    ++f.state.drawRefusals[static_cast<std::size_t>(DrawRefusal::Camera)];
    check(f.tick(5000),"camera refusal growth during active coverage is relevant");
    ++f.state.drawRefusals[static_cast<std::size_t>(DrawRefusal::ExactPicture)];
    check(f.tick(10000),"exact misses report even when metadata is stalled");
    ++f.state.drawRefusals[static_cast<std::size_t>(DrawRefusal::ExactPicture)];
    check(!f.tick(10001),"same-category error increase cannot bypass cadence");
    check(f.tick(15000),"a later increase in an already-known error remains observable");
    check(!f.tick(20000)&&!f.tick(45000),"unchanged counters do not produce idle logs");
    ++f.state.uploadRefused;check(f.tick(50000),"GPU upload refusal without video progress reports");
    ++f.state.submitRefusals[static_cast<std::size_t>(SubmitRefusal::Flags)];
    check(f.tick(55000),"unsupported submit delta reports");
    ++f.state.descriptorRefusals[static_cast<std::size_t>(DescriptorRefusal::MissingView)];
    check(f.tick(60000),"descriptor failure delta reports");
    f.state.lost=true;check(f.tick(65000),"device loss reports");
    f.state.lost=false;check(f.tick(70000),"device status recovery reports");
    ++f.state.guardAttempts;check(f.tick(100000),"native guard progress can drive a healthy snapshot without new metadata");
}
}
int main(){try{
    samplingAndStartup();healthyLifetime();anomaliesAndIdle();
    std::printf("renderer cold report policy: %u checks passed\n",checks);return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
