#pragma once
#include "bridge/stream_bridge_protocol.h"
#include <array>
#include <atomic>
#include <algorithm>
namespace stream_bridge::sender {
// Diagnostic samples only. Never used as source/frame/eye authority or as a
// substitute mask. Fixed 32K work ceiling; no image/RGB/file exports.
struct MaskContentSample {
    std::uint64_t sequence=0,frame=0;
    std::uint32_t pid=0,width=0,height=0,encodedBytes=0,encoding=0,rleBytes=0;
    std::uint32_t samples=0,scene=0,real=0,fractional=0,distinct=0,horizontal=0,vertical=0;
};
inline bool inspectMaskContent(Bytes raw,std::uint32_t width,std::uint32_t height,MaskContentSample& out)noexcept{
    if(!raw.data||!width||!height||width>8192||height>8192||raw.size!=std::size_t(width)*height)return false;
    std::array<bool,256> values{};MaskContentSample s=out;
    s.samples=s.scene=s.real=s.fractional=s.distinct=s.horizontal=s.vertical=0;
    const auto step=std::max<std::size_t>(1,(raw.size+32767)/32768);
    for(std::size_t i=0;i<raw.size;i+=step){const auto v=raw.data[i];++s.samples;
        if(v==255)++s.scene;else if(v==0)++s.real;else ++s.fractional;
        if(!values[v]){values[v]=true;++s.distinct;}
        if(i%width&&v!=raw.data[i-1])++s.horizontal;
        if(i>=width&&v!=raw.data[i-width])++s.vertical;
    }
    s.width=width;s.height=height;out=s;return true;
}
class MaskContentPublication {
public:
    // One encoded sample every 128 eligible masks. Count is diagnostic only.
    bool due()noexcept{return (calls_.fetch_add(1,std::memory_order_relaxed)&127)==0;}
    void publish(const MaskContentSample& s)noexcept{
        if(writer_.test_and_set(std::memory_order_acquire))return;
        sequence_.fetch_add(1);
        const auto pair=[](std::uint32_t a,std::uint32_t b){return (std::uint64_t(a)<<32)|b;};
        data_[0].store(s.frame);data_[1].store(pair(s.pid,s.width));data_[2].store(pair(s.height,s.encodedBytes));
        data_[3].store(pair(s.encoding,s.samples));data_[4].store(pair(s.scene,s.real));
        data_[5].store(pair(s.fractional,s.distinct));data_[6].store(pair(s.horizontal,s.vertical));data_[7].store(s.rleBytes);
        sequence_.fetch_add(1);writer_.clear(std::memory_order_release);
    }
    bool read(MaskContentSample& s)const noexcept{
        const auto before=sequence_.load();if(!before||(before&1))return false;
        std::array<std::uint64_t,8> d{};for(unsigned i=0;i<d.size();++i)d[i]=data_[i].load();
        if(before!=sequence_.load())return false;
        s.sequence=before;s.frame=d[0];s.pid=std::uint32_t(d[1]>>32);s.width=std::uint32_t(d[1]);
        s.height=std::uint32_t(d[2]>>32);s.encodedBytes=std::uint32_t(d[2]);s.encoding=std::uint32_t(d[3]>>32);s.samples=std::uint32_t(d[3]);
        s.scene=std::uint32_t(d[4]>>32);s.real=std::uint32_t(d[4]);s.fractional=std::uint32_t(d[5]>>32);s.distinct=std::uint32_t(d[5]);
        s.horizontal=std::uint32_t(d[6]>>32);s.vertical=std::uint32_t(d[6]);s.rleBytes=std::uint32_t(d[7]);return s.samples!=0;
    }
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"mask diagnostics must not block render work");
    std::atomic<std::uint64_t> calls_{},sequence_{};
    std::atomic_flag writer_=ATOMIC_FLAG_INIT;
    std::array<std::atomic<std::uint64_t>,8> data_{};
};
} // namespace stream_bridge::sender
