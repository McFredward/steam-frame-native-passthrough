// Explicit diagnostic only. No scene submission, setting writes or autostart.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace projection_probe {
using Handle = std::uint64_t;
struct Pixel { std::uint8_t r, g, b, a; };
constexpr unsigned PatternSize = 512;

inline std::vector<Pixel> pattern(unsigned eye) {
    if (eye > 1) return {};
    std::vector<Pixel> result(PatternSize * PatternSize);
    for (unsigned y = 0; y < PatternSize; ++y) {
        for (unsigned x = 0; x < PatternSize; ++x) {
            Pixel p{}; // transparent, premultiplied black
            if (x >= 64 && x < 448 && y >= 64 && y < 448) {
                const unsigned band = (x - 64) / 96;
                p.a = static_cast<std::uint8_t>((band + 1) * 255 / 4);
                // Left yellow, right cyan; alpha and both eye IDs are independent.
                p.r = eye == 0 ? p.a : 0;
                p.g = p.a;
                p.b = eye == 1 ? p.a : 0;
                if (y >= 320 && y < 384) p.r = p.g = p.b = 0;
                // A small full-alpha eye mark and one-pixel detail bars.
                if (y >= 96 && y < 128 && x < 96 + 32 * (eye + 1))
                    p = {255,255,255,255};
                if (y >= 416 && (x & 1u)) p = {255,255,255,255};
            }
            result[y * PatternSize + x] = p;
        }
    }
    return result;
}

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
        if (!explicitConsoleOptIn || attempted_) return false;
        attempted_ = true;
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
        active_ = false;
        attempted_ = false;
        api_.shutdown();
        return cleanupOk_;
    }
    bool active() const { return active_; }
private:
    bool failed() { stop(); return false; }
    Api& api_;
    std::array<Handle,2> handles_{};
    bool attempted_ = false, active_ = false, cleanupOk_ = true;
};
} // namespace projection_probe
