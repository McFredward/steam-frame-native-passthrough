#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
namespace stream_bridge::sender {
struct SourceAlphaMetadata {
    std::uint64_t present=0;
    std::uint32_t pid=0,width=0,height=0,resourceFormat=0,viewFormat=0;
    std::array<float,4> uv{};
    float nonFovealAlpha=0,directFoveated=0,sharpness=0;
};
struct SourceAlphaSample {
    SourceAlphaMetadata source{};
    std::uint64_t samples=0;
    std::array<std::uint64_t,3> alpha{}; // min/mean/max (8 each), zero/partial (16 each)
};
// Coherent diagnostic publication only. None of these values can grant a source,
// attach a mask to video or substitute for the native frame/eye/encoder proof.
class SourceAlphaPublication {
public:
    void publish(const SourceAlphaSample& sample)noexcept{
        const auto bits=[](float value){std::uint32_t out=0;std::memcpy(&out,&value,4);return out;};
        const auto pair=[](std::uint32_t a,std::uint32_t b){return (std::uint64_t(a)<<32)|b;};
        // Rare (once per eye / two seconds), single writer. Sequentially
        // consistent atomics also make a partially read publication rejectable
        // without a mutex, data race, spin loop or torn 64-bit diagnostic.
        sequence_.fetch_add(1);
        data_[0].store(sample.samples);data_[1].store(sample.source.present);
        data_[2].store(pair(sample.source.pid,sample.source.width));
        data_[3].store(pair(sample.source.height,sample.source.resourceFormat));
        data_[4].store(pair(sample.source.viewFormat,bits(sample.source.sharpness)));
        data_[5].store(pair(bits(sample.source.uv[0]),bits(sample.source.uv[1])));
        data_[6].store(pair(bits(sample.source.uv[2]),bits(sample.source.uv[3])));
        data_[7].store(pair(bits(sample.source.nonFovealAlpha),bits(sample.source.directFoveated)));
        for(unsigned i=0;i<3;++i)data_[8+i].store(sample.alpha[i]);
        sequence_.fetch_add(1);
    }
    bool read(SourceAlphaSample& sample)const noexcept{
        const auto before=sequence_.load();if(!before||(before&1))return false;
        std::array<std::uint64_t,11> data{};for(unsigned i=0;i<data.size();++i)data[i]=data_[i].load();
        if(sequence_.load()!=before)return false;
        const auto value=[](std::uint32_t bits){float out=0;std::memcpy(&out,&bits,4);return out;};
        sample.samples=data[0];sample.source.present=data[1];
        sample.source.pid=std::uint32_t(data[2]>>32);sample.source.width=std::uint32_t(data[2]);
        sample.source.height=std::uint32_t(data[3]>>32);sample.source.resourceFormat=std::uint32_t(data[3]);
        sample.source.viewFormat=std::uint32_t(data[4]>>32);sample.source.sharpness=value(std::uint32_t(data[4]));
        sample.source.uv={{value(std::uint32_t(data[5]>>32)),value(std::uint32_t(data[5])),value(std::uint32_t(data[6]>>32)),value(std::uint32_t(data[6]))}};
        sample.source.nonFovealAlpha=value(std::uint32_t(data[7]>>32));sample.source.directFoveated=value(std::uint32_t(data[7]));
        for(unsigned i=0;i<3;++i)sample.alpha[i]=data[8+i];
        return sample.samples!=0;
    }
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"source diagnostics must not lock");
    std::atomic<std::uint64_t> sequence_{};
    std::array<std::atomic<std::uint64_t>,11> data_{};
};
} // namespace stream_bridge::sender
