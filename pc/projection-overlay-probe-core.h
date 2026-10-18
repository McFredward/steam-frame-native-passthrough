// Explicit diagnostic only. No scene submission, setting writes or autostart.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>

namespace projection_probe {
using Handle = std::uint64_t;
struct Pixel { std::uint8_t r, g, b, a; };
constexpr unsigned PatternSize = 2048;
constexpr float QuadDistance = 1.2f, QuadWidth = .6f;
constexpr unsigned LogCap = 8192;
constexpr const char* ReadyLine = "SFNP_PROJECTION_READY_V2\n";
constexpr const char* StoppedLine = "SFNP_PROJECTION_STOPPED_V2\n";

// Both eyes see the same mono image on one common physical world plane.
// The inner zero-RGB/different-alpha lanes remain an alpha witness only when a
// separate, explicit experiment establishes the underlying color/coverage base.
inline Pixel patternPixel(unsigned eye, unsigned x, unsigned y) {
    if (eye > 1 || x >= PatternSize || y >= PatternSize) return {};
    Pixel p{};
    if (x >= 256 && x < 1792 && y >= 256 && y < 1536) {
        const unsigned band = (x - 256) / 192;
        constexpr std::array<std::uint8_t,8> alpha{{0,64,128,255,64,128,192,255}};
        p.a = alpha[band];
        if (band == 4 || band == 5) p.r = p.g = p.b = p.a / 2;
        if (band >= 6) { p.g = p.a; (eye == 0 ? p.r : p.b) = p.a; }
    }
    if (x >= 960 && x < 1088 && y >= 960 && y < 1088) p = {192,192,192,255};
    if (y >= 128 && y < 192 && x >= 256 && x < 256 + 64 * (eye + 1)) p = {255,255,255,255};
    if (y >= 1600 && y < 1936 && x >= 256 && x < 1792) {
        const unsigned period = 1u << ((y - 1600) / 48);
        p = (x / period) % 2 ? Pixel{255,255,255,255} : Pixel{0,0,0,255};
    }
    return p;
}
inline std::vector<Pixel> pattern(unsigned eye) {
    if (eye > 1) return {};
    std::vector<Pixel> out(PatternSize * PatternSize);
    for (unsigned y=0; y<PatternSize; ++y) for (unsigned x=0; x<PatternSize; ++x)
        out[y*PatternSize+x] = patternPixel(eye,x,y);
    return out;
}
// Strictly bounded trusted ASCII metadata; never native free-form strings.
struct LogSink { virtual ~LogSink() = default; virtual bool write(const std::string&) = 0; };
class Log {
public:
    explicit Log(LogSink& sink) : sink_(sink) {}
    bool emit(const std::string& line) {
        if (failed_ || line.empty() || line.back() != '\n' || line.size() > 1024 || line.size() > LogCap - bytes_) {
            failed_ = true; return false;
        }
        for (unsigned char c : line) if ((c < 32 && c != '\n') || c > 126) { failed_ = true; return false; }
        if (!sink_.write(line)) { failed_ = true; return false; }
        bytes_ += static_cast<unsigned>(line.size()); return true;
    }
    unsigned bytes() const { return bytes_; }
    bool failed() const { return failed_; }
private:
    LogSink& sink_; unsigned bytes_ = 0; bool failed_ = false;
};

struct Matrix { float m[3][4]{}; };
inline bool finite(const Matrix& v) {
    for (const auto& row : v.m) for (float n : row) if (!std::isfinite(n)) return false;
    return true;
}
inline Matrix compose(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned k = 0; k < 3; ++k) out.m[r][c] += a.m[r][k] * b.m[k][c];
        out.m[r][3] = a.m[r][3];
        for (unsigned k = 0; k < 3; ++k) out.m[r][3] += a.m[r][k] * b.m[k][3];
    }
    return out;
}

inline Matrix quadPose(const Matrix& head) {
    Matrix offset{};
    offset.m[0][0] = offset.m[1][1] = offset.m[2][2] = 1;
    offset.m[2][3] = -QuadDistance;
    return compose(head,offset);
}

// This adapter owns exactly one ordinary mono overlay and its SDK/D3D objects.
// The public API's accepted local state never proves what a receiver displays.
struct Api {
    virtual ~Api() = default;
    virtual bool initialize() = 0;
    virtual bool create(Handle& handle) = 0;
    virtual bool configure(Handle handle) = 0;
    virtual bool texture(Handle handle) = 0;
    virtual bool flush() = 0;
    virtual bool submit(Handle handle) = 0;
    virtual bool show(Handle handle) = 0;
    virtual bool reveal(Handle handle) = 0;
    virtual bool verify(Handle handle) = 0;
    virtual bool ready() = 0;
    virtual void stopped(bool clean) = 0;
    virtual bool hide(Handle handle) = 0;
    virtual bool clear(Handle handle) = 0;
    virtual bool destroy(Handle handle) = 0;
    virtual void shutdown() = 0;
};
class Session {
public:
    explicit Session(Api& api) : api_(api) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { stop(); }
    bool start(bool explicitOptIn) {
        if (!explicitOptIn || used_) return false;
        used_ = attempted_ = true;
        if (!api_.initialize()) return failed();
        if (!api_.create(handle_) || !handle_) return failed();
        if (!api_.configure(handle_) || !api_.texture(handle_) || !api_.flush()
            || !api_.submit(handle_) || !api_.show(handle_) || !api_.reveal(handle_) || !api_.verify(handle_) || !api_.ready()) return failed();
        active_ = true; return true;
    }
    bool stop() {
        if (!attempted_) return cleanupOk_;
        if (handle_) {
            cleanupOk_ = api_.hide(handle_) && cleanupOk_;
            cleanupOk_ = api_.clear(handle_) && cleanupOk_;
            cleanupOk_ = api_.destroy(handle_) && cleanupOk_;
        }
        handle_ = 0;
        const bool hadReady = active_; active_ = false; attempted_ = false;
        api_.shutdown();
        if (hadReady) api_.stopped(cleanupOk_);
        return cleanupOk_;
    }
    bool active() const { return active_; }
private:
    bool failed() { stop(); return false; }
    Api& api_; Handle handle_ = 0;
    bool used_ = false, attempted_ = false, active_ = false, cleanupOk_ = true;
};
} // namespace projection_probe
