#pragma once
#include "stream_bridge_renderer.h"
#include <chrono>

namespace stream_bridge_renderer {
// Cold-worker diagnostics only. The sampler may take the GPU pool lock, so
// reject frequent ticks before calling it. There is no lifetime report budget.
class ColdReportPolicy {
public:
    using Clock=std::chrono::steady_clock;
    template<class Sampler>
    bool inspect(Clock::time_point now,Sampler&& sample,RendererStats& output) noexcept {
        if(sampled_&&now<nextSample_)return false;
        sampled_=true;nextSample_=now+std::chrono::seconds(5);
        const auto current=sample();
        if(!current.metadata&&!current.pictures)return false;
        const bool progress=current.metadata!=reported_.metadata||current.guardAttempts!=reported_.guardAttempts
            ||current.opaqueSubmits!=reported_.opaqueSubmits;
        if(reportedOnce_&&!anomaly(current)&&!(now>=nextHealthy_&&progress))return false;
        output=current;reported_=current;reportedOnce_=true;nextHealthy_=now+std::chrono::seconds(30);
        return true;
    }
private:
    bool anomaly(const RendererStats& s)const noexcept {
        const auto& p=reported_;
        if(s.maskRejected!=p.maskRejected||s.uploadRefused!=p.uploadRefused||s.guardRefused!=p.guardRefused
            ||s.imageCapacityRefused!=p.imageCapacityRefused||s.memoryCapacityRefused!=p.memoryCapacityRefused
            ||s.viewCapacityRefused!=p.viewCapacityRefused||s.descriptorCapacityRefused!=p.descriptorCapacityRefused
            ||s.descriptorAllocationRefused!=p.descriptorAllocationRefused||s.descriptorUnknownUpdates!=p.descriptorUnknownUpdates
            ||s.poolRefused!=p.poolRefused||s.quarantined!=p.quarantined||s.lost!=p.lost
            ||s.prepared!=p.prepared||s.pipelines!=p.pipelines)return true;
        // Native non-target pipelines and empty native submissions are expected.
        // Camera-off draws are expected too; report their growth promptly when
        // active coverage has arrived since the last report, not during None.
        const bool activeCoverage=s.maskDecoded!=p.maskDecoded;
        for(std::size_t n=0;n<s.drawRefusals.size();++n){
            if(n==static_cast<std::size_t>(DrawRefusal::Pipeline))continue;
            if(n==static_cast<std::size_t>(DrawRefusal::Camera)&&!activeCoverage)continue;
            if(s.drawRefusals[n]!=p.drawRefusals[n])return true;
        }
        for(std::size_t n=0;n<s.collectRefusals.size();++n){
            if(n==static_cast<std::size_t>(CollectRefusal::Empty))continue;
            if(n==static_cast<std::size_t>(CollectRefusal::Camera)&&!activeCoverage)continue;
            if(s.collectRefusals[n]!=p.collectRefusals[n])return true;
        }
        return s.submitRefusals!=p.submitRefusals||s.descriptorRefusals!=p.descriptorRefusals;
    }
    Clock::time_point nextSample_{},nextHealthy_{};
    RendererStats reported_{};
    bool sampled_=false,reportedOnce_=false;
};
}
