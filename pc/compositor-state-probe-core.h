// Read-only public state observer; no renderer, setters or runtime autostart.
#pragma once
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace compositor_probe {
constexpr unsigned MaxSamples = 32, MaxLineBytes = 1024, MaxOutputBytes = 32768;
constexpr unsigned TerminalReserve = 256;
constexpr const char* Schema = "sfnp-pc-compositor-state-v1";
enum class Stage : int { Arguments = 1, Library, Exports, Background,
    System, Compositor, Overlay, Settings, Functions, Stdin, Exception };
struct Status { Stage stage = Stage::Background; std::int32_t code = 0; };
struct Color { float r = 0, g = 0, b = 0, a = 0; };
template<class T> struct Value { T value{}; std::int32_t error = 0; };
struct Text { std::string value; std::int32_t error = 0; bool valid = true; };
enum class Property { Tracking, Model, Registration };
enum class BoolSetting { DoNotFadeToGrid, ForceFadeOnBadTracking, StandaloneSystemLayer };

// These narrow getters are the only runtime operations exposed to the monitor.
// The Win64 adapter owns SDK loading/Background initialization and partial cleanup.
struct Api {
    virtual ~Api() = default;
    virtual bool initialize(Status&) = 0;
    virtual std::uint64_t nowMs() = 0;
    virtual Color fade(bool background) = 0;
    virtual float gridAlpha() = 0;
    virtual bool dashboardVisible() = 0;
    virtual bool canRenderScene() = 0;
    virtual std::uint32_t sceneFocus() = 0;
    virtual std::uint32_t lastRenderer() = 0;
    virtual Value<bool> boolean(BoolSetting) = 0;
    virtual Value<float> sceneDimming() = 0;
    virtual bool connected() = 0;
    virtual std::int32_t deviceClass() = 0;
    virtual Text property(Property) = 0;
    virtual void shutdown() = 0;
};
struct Sink { virtual ~Sink() = default; virtual bool write(const std::string&) = 0; };
class Output {
public:
    explicit Output(Sink& sink) : sink_(sink) {}
    bool emit(const std::string& line) {
        if (failed_ || line.empty() || line.back() != '\n' || line.size() > MaxLineBytes
            || line.size() > MaxOutputBytes - bytes_) { failed_ = true; return false; }
        for (unsigned char c : line) if ((c < 32 && c != '\n') || c > 126) { failed_ = true; return false; }
        if (!sink_.write(line)) { failed_ = true; return false; }
        bytes_ += static_cast<unsigned>(line.size()); return true;
    }
    unsigned bytes() const { return bytes_; }
    bool failed() const { return failed_; }
private:
    Sink& sink_; unsigned bytes_ = 0; bool failed_ = false;
};
inline std::string prefix(const char* event) {
    return std::string("{\"schema\":\"") + Schema + "\",\"event\":\"" + event + "\"";
}
inline bool refusal(Output& output, Status status) {
    return output.emit(prefix("refused") + ",\"stage\":" + std::to_string(static_cast<int>(status.stage))
        + ",\"code\":" + std::to_string(status.code) + "}\n");
}
inline bool bounded(const Text& t) {
    if (!t.valid || t.error || t.value.size() > 96) return false;
    for (unsigned char c : t.value) if (c < 32 || c > 126) return false;
    return true;
}
inline bool optional(const Text& t) { return bounded(t) || t.error == 4; }
inline std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}
inline bool same(const Text& a, const Text& b) {
    return a.error == b.error && bounded(a) == bounded(b)
        && (!bounded(a) || a.value == b.value);
}
struct Identity {
    bool connected = false; std::int32_t deviceClass = 0;
    Text tracking, model, registration;
};
inline Identity identity(Api& api) {
    Identity result;
    result.connected = api.connected(); result.deviceClass = api.deviceClass();
    result.tracking = api.property(Property::Tracking);
    result.model = api.property(Property::Model);
    result.registration = api.property(Property::Registration);
    return result;
}
inline bool same(const Identity& a, const Identity& b) {
    return a.connected == b.connected && a.deviceClass == b.deviceClass
        && same(a.tracking,b.tracking) && same(a.model,b.model) && same(a.registration,b.registration);
}
inline bool valid(const Identity& v) {
    return v.connected && v.deviceClass == 1 && optional(v.tracking)
        && bounded(v.model) && !v.model.value.empty() && optional(v.registration);
}
// Cooperative public identity, not receiver verification or hardware attestation.
// Match the existing GloomhavenVR canonical and observed691 profiles exactly.
inline unsigned profile(const Identity& v, bool stable) {
    if (!stable || !valid(v)) return 0;
    const std::string model = lower(v.model.value);
    const std::string tracking = v.tracking.error ? "" : lower(v.tracking.value);
    const std::string registered = v.registration.error ? "" : lower(v.registration.value);
    const bool link = registered == "vrlink" || registered.compare(0,7,"vrlink/") == 0;
    if (model == "steam frame" && (tracking == "vrlink" || link)) return 1;
    if (model == "deckard mp" && tracking == "cv" && (registered.empty() || link)) return 2;
    return 0;
}
inline float finite(float value, bool& all) {
    if (!std::isfinite(value)) { all = false; return 0; }
    return value == 0 ? 0 : value;
}

class Monitor {
public:
    Monitor(Api& api, Output& output) : api_(api), output_(output) {}
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    ~Monitor() { stop(); }
    bool start(bool explicitLaunch) {
        if (!explicitLaunch || used_) return false;
        used_ = attempted_ = true;
        Status status;
        if (!api_.initialize(status)) { refusal(output_,status); stop(); return false; }
        active_ = output_.emit(prefix("ready") + ",\"read_only\":true,\"non_atomic\":true}\n");
        readyEmitted_ = active_;
        if (!active_) stop();
        return active_;
    }
    void tick() {
        if (!active_ || budget_ || output_.failed()) return;
        const auto begin = api_.nowMs();
        const Color fg = api_.fade(false), bg = api_.fade(true);
        const float grid = api_.gridAlpha();
        const bool dashboard = api_.dashboardVisible(), canRender = api_.canRenderScene();
        const auto focus = api_.sceneFocus(), renderer = api_.lastRenderer();
        const auto noGrid = api_.boolean(BoolSetting::DoNotFadeToGrid);
        const auto trackingFade = api_.boolean(BoolSetting::ForceFadeOnBadTracking);
        const auto dimming = api_.sceneDimming();
        const auto standalone = api_.boolean(BoolSetting::StandaloneSystemLayer);
        const Identity first = identity(api_), second = identity(api_);
        const auto end = api_.nowMs();
        const bool stable = same(first,second);
        bool numbers = true;
        std::ostringstream data; data.imbue(std::locale::classic()); data << std::setprecision(9) << std::boolalpha;
        data << ",\"fg_r\":" << finite(fg.r,numbers) << ",\"fg_g\":" << finite(fg.g,numbers)
            << ",\"fg_b\":" << finite(fg.b,numbers) << ",\"fg_a\":" << finite(fg.a,numbers)
            << ",\"bg_r\":" << finite(bg.r,numbers) << ",\"bg_g\":" << finite(bg.g,numbers)
            << ",\"bg_b\":" << finite(bg.b,numbers) << ",\"bg_a\":" << finite(bg.a,numbers)
            << ",\"grid_alpha\":" << finite(grid,numbers)
            << ",\"dashboard_visible\":" << dashboard << ",\"can_render_scene\":" << canRender
            << ",\"scene_focus_pid\":" << focus << ",\"last_frame_renderer_pid\":" << renderer
            << ",\"do_not_fade_to_grid\":" << (!noGrid.error && noGrid.value)
            << ",\"do_not_fade_to_grid_error\":" << noGrid.error
            << ",\"force_fade_tracking\":" << (!trackingFade.error && trackingFade.value)
            << ",\"force_fade_tracking_error\":" << trackingFade.error
            << ",\"scene_dimming\":" << (dimming.error ? 0 : finite(dimming.value,numbers))
            << ",\"scene_dimming_error\":" << dimming.error
            << ",\"standalone_system_layer\":" << (!standalone.error && standalone.value)
            << ",\"standalone_system_layer_error\":" << standalone.error
            << ",\"hmd_connected\":" << first.connected << ",\"hmd_class\":" << first.deviceClass
            << ",\"identity_stable\":" << stable << ",\"identity_valid\":" << (stable && valid(first))
            << ",\"frame_link_profile\":" << profile(first,stable)
            << ",\"tracking_error\":" << first.tracking.error << ",\"model_error\":" << first.model.error
            << ",\"registration_error\":" << first.registration.error
            << ",\"numeric_valid\":" << numbers;
        const std::string body = data.str();
        if (body == previous_) return; // Read-span timestamps never defeat deduplication.
        const std::string line = prefix("sample") + ",\"sample\":" + std::to_string(samples_ + 1)
            + ",\"begin_ms\":" + std::to_string(begin) + ",\"end_ms\":" + std::to_string(end) + body + "}\n";
        if (line.size() > MaxLineBytes || line.size() + output_.bytes() > MaxOutputBytes - TerminalReserve) {
            budget(); return;
        }
        if (output_.emit(line)) { previous_ = body; if (++samples_ == MaxSamples) budget(); }
    }
    void stop() {
        if (!attempted_) return;
        active_ = false; attempted_ = false;
        api_.shutdown(); // End only this Background client, never the runtime.
        if (readyEmitted_) {
            readyEmitted_ = false;
            output_.emit(prefix("stopped") + ",\"samples\":" + std::to_string(samples_) + "}\n");
        }
    }
    bool active() const { return active_; }
    bool idle() const { return budget_ || output_.failed(); }
    unsigned samples() const { return samples_; }
private:
    void budget() {
        budget_ = true;
        output_.emit(prefix("budget") + ",\"samples\":" + std::to_string(samples_)
            + ",\"bytes\":" + std::to_string(output_.bytes()) + "}\n");
    }
    Api& api_; Output& output_;
    bool used_ = false, attempted_ = false, active_ = false, budget_ = false, readyEmitted_ = false;
    unsigned samples_ = 0; std::string previous_;
};
} // namespace compositor_probe
