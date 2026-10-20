#include "pc/stream-bridge-source-alpha-stats.h"
#include <atomic>
#include <cstdio>
#include <thread>
using namespace stream_bridge::sender;
int main(){
    SourceAlphaPublication publication;SourceAlphaSample sample{};
    if(publication.read(sample))return 1;
    std::atomic<bool> finished{false};
    std::thread writer([&]{for(std::uint64_t n=1;n<=100000;++n){
        SourceAlphaSample next{};next.samples=n;next.source.present=n;next.source.pid=std::uint32_t(n);
        next.source.width=std::uint32_t(n);next.source.height=std::uint32_t(n);next.source.resourceFormat=27;next.source.viewFormat=29;
        next.source.uv={{float(n),0,1,1}};next.source.nonFovealAlpha=float(n);next.alpha.fill(n);publication.publish(next);
        if(n%256==0)std::this_thread::yield();
    }finished.store(true);});
    bool okay=true;unsigned reads=0;
    while(!finished.load())if(publication.read(sample)){
        ++reads;const auto n=sample.samples;
        okay=okay&&sample.source.present==n&&sample.source.pid==n&&sample.source.width==n&&sample.source.height==n
            &&sample.source.uv[0]==float(n)&&sample.source.nonFovealAlpha==float(n)&&sample.source.resourceFormat==27&&sample.source.viewFormat==29;
        for(auto value:sample.alpha)okay=okay&&value==n;
    }
    writer.join();okay=okay&&publication.read(sample)&&sample.samples==100000;
    if(!okay)return 2;
    std::printf("source alpha diagnostic publication: coherent across 100000 writes and %u accepted concurrent reads\n",reads);
    return 0;
}
