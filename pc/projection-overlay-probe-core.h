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
constexpr float ChartMin = .40f, ChartMax = .60f;
constexpr unsigned LogCap = 8192;
constexpr const char* ReadyLine = "SFNP_PROJECTION_READY_V2\n";
constexpr const char* StoppedLine = "SFNP_PROJECTION_STOPPED_V2\n";

// The INNER transparent/black-alpha lanes, not the outer edge, are the witness.
// Opaque black contributes zero RGB yet must hide the camera if alpha survives.
inline std::vector<Pixel> pattern(unsigned eye) {
    if (eye > 1) return {};
    std::vector<Pixel> result(PatternSize * PatternSize);
    for (unsigned y = 0; y < PatternSize; ++y) for (unsigned x = 0; x < PatternSize; ++x) {
        Pixel p{};
        if (x >= 256 && x < 1792 && y >= 256 && y < 1536) {
            const unsigned band = (x - 256) / 192;
            constexpr std::array<std::uint8_t,8> alpha{{0,64,128,255,64,128,192,255}};
            p.a = alpha[band];
            if (band == 4 || band == 5) p.r = p.g = p.b = p.a / 2;
            if (band >= 6) { p.g = p.a; (eye == 0 ? p.r : p.b) = p.a; }
        }
        // Left has one white block, right two; IDs occupy the interior chart.
        if (y >= 128 && y < 192 && x >= 256 && x < 256 + 64 * (eye + 1)) p = {255,255,255,255};
        if (y >= 1600 && y < 1792 && x >= 256 && x < 1792) {
            const unsigned period = y < 1664 ? 1 : y < 1728 ? 2 : 4;
            p = (x / period) % 2 ? Pixel{255,255,255,255} : Pixel{0,0,0,255};
        }
        result[y * PatternSize + x] = p;
    }
    return result;
}
struct Frustum { float left = 0, right = 0, top = 0, bottom = 0; };
inline bool finite(const Frustum& f) {
    return std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.top)
        && std::isfinite(f.bottom) && f.left < f.right && f.top < f.bottom;
}
inline bool central(const Frustum& full, Frustum& out) {
    if (!finite(full)) return false;
    const float width = full.right - full.left, height = full.bottom - full.top;
    out = {full.left + width * ChartMin, full.left + width * ChartMax,
        full.top + height * ChartMin, full.top + height * ChartMax};
    return finite(out);
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

// The adapter owns SDK/D3D objects. shutdown() releases even a partially failed
// initialize(); handles returned on failed creation are still caller-owned.
struct Api {
    virtual ~Api() = default;
    virtual bool initialize() = 0;
    virtual bool create(unsigned eye, Handle& handle) = 0;
    virtual bool configure(unsigned eye, Handle handle) = 0;
    virtual bool texture(unsigned eye, Handle handle) = 0;
    virtual bool show(Handle handle) = 0;
    virtual bool reveal(Handle handle) = 0;
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
    bool start(bool explicitConsoleOptIn) {
        if (!explicitConsoleOptIn || used_) return false;
        used_ = attempted_ = true;
        if (!api_.initialize()) return failed();
        for (unsigned eye = 0; eye < 2; ++eye) {
            if (!api_.create(eye, handles_[eye]) || !handles_[eye]) return failed();
            if (eye == 1 && handles_[1] == handles_[0]) return failed();
        }
        // Prepare both texture/pose cohorts before either becomes visible.
        for (unsigned eye = 0; eye < 2; ++eye)
            if (!api_.configure(eye, handles_[eye])) return failed();
        for (unsigned eye = 0; eye < 2; ++eye)
            if (!api_.texture(eye, handles_[eye])) return failed();
        for (Handle handle : handles_) if (!api_.show(handle)) return failed();
        for (Handle handle : handles_) if (!api_.reveal(handle)) return failed();
        if (!api_.ready()) return failed();
        active_ = true;
        return true;
    }
    bool stop() {
        if (!attempted_) return cleanupOk_;
        // OpenVR has no atomic two-overlay publish; this is a transport probe,
        // not a production stereo presentation implementation.
        if (handles_[1] == handles_[0]) handles_[1] = 0;
        for (Handle handle : handles_) if (handle) cleanupOk_ = api_.hide(handle) && cleanupOk_;
        for (Handle handle : handles_) if (handle) cleanupOk_ = api_.clear(handle) && cleanupOk_;
        for (Handle handle : handles_) if (handle) cleanupOk_ = api_.destroy(handle) && cleanupOk_;
        handles_ = {};
        const bool hadReady = active_;
        active_ = false;
        attempted_ = false;
        api_.shutdown();
        if (hadReady) api_.stopped(cleanupOk_);
        return cleanupOk_;
    }
    bool active() const { return active_; }
private:
    bool failed() { stop(); return false; }
    Api& api_;
    std::array<Handle,2> handles_{};
    bool used_ = false, attempted_ = false, active_ = false, cleanupOk_ = true;
};
} // namespace projection_probe
