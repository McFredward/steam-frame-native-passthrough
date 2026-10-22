#pragma once
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace stream_bridge::logging {
enum class Level : unsigned { Error, Warning, Important, Debug };
inline std::atomic<Level> current{Level::Important};
inline bool enabled(Level level) noexcept {return unsigned(level)<=unsigned(current.load(std::memory_order_relaxed));}
inline bool debug() noexcept {return enabled(Level::Debug);}
inline bool parse(const char* text,Level& value) noexcept {
    if(!text)return false;
    constexpr const char* names[]{"error","warning","important","debug"};
    for(unsigned n=0;n<4;++n){
        unsigned i=0;for(;names[n][i]&&text[i];++i){const char c=text[i]>='A'&&text[i]<='Z'?char(text[i]+('a'-'A')):text[i];if(c!=names[n][i])break;}
        if(!names[n][i]&&(!text[i]||(text[i]=='\n'&&!text[i+1])||(text[i]=='\r'&&text[i+1]=='\n'&&!text[i+2]))){value=static_cast<Level>(n);return true;}
    }
    return false;
}
inline bool readCold(const std::filesystem::path& path,Level& level) {
    std::ifstream file(path,std::ios::binary);char text[32]{};
    if(!file)return false;
    file.read(text,sizeof(text)-1);if(!file.eof())return false;
    return parse(text,level);
}
// Startup/worker initialization only. Rendering/decoder callbacks only query
// the atomic level; no file reads, environment lookups, locks or allocations.
inline void configureCold(const std::filesystem::path& defaults,const std::filesystem::path& overridePath) noexcept {
    Level level=Level::Important;
    try {readCold(defaults,level);readCold(overridePath,level);}catch(...){/* Logging never changes native admission. */}
    Level overrideLevel{};if(parse(std::getenv("SFNP_LOG_LEVEL"),overrideLevel))level=overrideLevel;
    current.store(level,std::memory_order_relaxed);
}
}
