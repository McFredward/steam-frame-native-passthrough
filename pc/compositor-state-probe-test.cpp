#include "compositor-state-probe-core.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace compositor_probe;
unsigned checks = 0;
void require(bool value, const char* why) {
    ++checks; if (!value) { std::fprintf(stderr,"FAIL %s\n",why); std::exit(1); }
}
struct Captured final : Sink {
    bool fail = false; std::vector<std::string> lines;
    bool write(const std::string& line) override { lines.push_back(line); return !fail; }
    unsigned count(const std::string& fragment) const {
        return static_cast<unsigned>(std::count_if(lines.begin(),lines.end(),[&](const std::string& s) {
            return s.find(fragment) != std::string::npos;
        }));
    }
};
struct Fake final : Api {
    bool initOk = true; Status failure{Stage::Background,121};
    bool dashboard = false, canRender = true;
    std::uint32_t focus = 123, renderer = 123;
    std::uint64_t time = 0;
    Color fg{}, bg{}; float grid = 0;
    Value<bool> noGrid{}, trackingFade{}, standalone{}; Value<float> dimming{1,0};
    Identity hmd{true,1,{"cv",0,true},{"Deckard MP",0,true},{"",0,true}};
    bool switching = false;
    std::vector<std::string> events;
    bool initialize(Status& result) override { events.push_back("init"); result = failure; return initOk; }
    std::uint64_t nowMs() override { events.push_back("clock"); return time++; }
    Color fade(bool background) override { events.push_back(background ? "fade-bg" : "fade-fg"); return background ? bg : fg; }
    float gridAlpha() override { events.push_back("grid"); return grid; }
    bool dashboardVisible() override { events.push_back("dashboard"); return dashboard; }
    bool canRenderScene() override { events.push_back("can-render"); return canRender; }
    std::uint32_t sceneFocus() override { events.push_back("focus"); return focus; }
    std::uint32_t lastRenderer() override { events.push_back("renderer"); return renderer; }
    Value<bool> boolean(BoolSetting setting) override {
        events.push_back("bool" + std::to_string(static_cast<int>(setting)));
        return setting == BoolSetting::DoNotFadeToGrid ? noGrid
            : setting == BoolSetting::ForceFadeOnBadTracking ? trackingFade : standalone;
    }
    Value<float> sceneDimming() override { events.push_back("dimming"); return dimming; }
    bool connected() override { events.push_back("connected"); return hmd.connected; }
    std::int32_t deviceClass() override { events.push_back("class"); return hmd.deviceClass; }
    Text property(Property id) override {
        events.push_back("property" + std::to_string(static_cast<int>(id)));
        if (switching && std::count(events.begin(),events.end(),"connected") % 2 == 0 && id == Property::Model)
            return {"Other HMD",0,true};
        return id == Property::Tracking ? hmd.tracking : id == Property::Model ? hmd.model : hmd.registration;
    }
    void shutdown() override { events.push_back("shutdown"); }
    unsigned count(const std::string& event) const {
        return static_cast<unsigned>(std::count(events.begin(),events.end(),event));
    }
};
std::vector<std::string> readOrder() {
    return {"clock","fade-fg","fade-bg","grid","dashboard","can-render","focus","renderer",
        "bool0","bool1","dimming","bool2","connected","class","property0","property1","property2",
        "connected","class","property0","property1","property2","clock"};
}
int transcript(bool refused = false, bool budget = false) {
    struct Printed final : Sink { bool write(const std::string& line) override {
        return std::fwrite(line.data(),1,line.size(),stdout) == line.size();
    }} sink;
    Fake api; api.initOk = !refused; Output out(sink); Monitor monitor(api,out);
    if (!monitor.start(true)) return refused ? 0 : 1;
    if (budget) {
        for (unsigned i = 0; i < MaxSamples; ++i) { api.focus = i; monitor.tick(); }
        monitor.tick(); monitor.stop(); return 0;
    }
    monitor.tick(); api.dashboard = true; api.fg.a = .75f; api.grid = .5f; monitor.tick();
    api.dimming = {std::numeric_limits<float>::quiet_NaN(),5}; monitor.tick();
    api.fg.r = std::numeric_limits<float>::infinity(); monitor.tick(); monitor.stop(); return 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--transcript") return transcript();
    if (argc == 2 && std::string(argv[1]) == "--refused-transcript") return transcript(true);
    if (argc == 2 && std::string(argv[1]) == "--budget-transcript") return transcript(false,true);
    {
        Fake api; Captured sink; Output out(sink); Monitor monitor(api,out);
        require(!monitor.start(false),"no implicit launch"); require(api.events.empty(),"no API call before opt-in");
        require(monitor.start(true),"explicit client starts");
        require(!monitor.start(true) && api.count("init") == 1,"duplicate start never initializes again");
        monitor.tick(); require(monitor.samples() == 1,"first snapshot appears immediately");
        const auto expected = readOrder();
        require(std::vector<std::string>(api.events.begin()+1,api.events.end()) == expected,"actual monitor reads both fades and all other getters in order");
        for (unsigned i = 0; i < 20; ++i) monitor.tick();
        require(monitor.samples() == 1,"clock-only changes do not defeat deduplication");
        require(sink.count("\"frame_link_profile\":2") == 1,"observed691 profile recorded without original strings");
        api.dashboard = true; monitor.tick(); require(monitor.samples() == 2,"dashboard transition emitted");
        api.fg.a = .5f; monitor.tick(); require(monitor.samples() == 3,"foreground fade transition emitted");
        api.bg.a = .25f; monitor.tick(); require(monitor.samples() == 4,"background fade transition emitted");
        api.grid = .125f; monitor.tick(); require(monitor.samples() == 5,"grid transition emitted");
        api.focus = 0; monitor.tick(); require(monitor.samples() == 6,"scene focus transition emitted");
        api.renderer = 456; monitor.tick(); require(monitor.samples() == 7,"renderer transition emitted");
        api.noGrid = {true,0}; monitor.tick(); require(monitor.samples() == 8,"settings readback change emitted");
        monitor.stop(); const auto events = api.events.size(); monitor.tick(); monitor.stop();
        require(api.events.size() == events && api.count("shutdown") == 1,"manual stop is idempotent and disarms reads");
        require(!monitor.start(true),"completed observer never restarts its client implicitly");
        require(sink.count("\"event\":\"stopped\"") == 1,"one terminal record");
    }
    for (int stage = 2; stage <= 9; ++stage) {
        Fake api; api.initOk = false; api.failure = {static_cast<Stage>(stage),121}; Captured sink; Output out(sink);
        { Monitor monitor(api,out); require(!monitor.start(true),"partial initialization refusal propagated");
          require(api.count("shutdown") == 1,"refusal cleans partial lifetime immediately");
          monitor.tick(); require(!monitor.active(),"refused initialization cannot poll"); }
        require(api.count("shutdown") == 1,"partial initialization lifetime reclaimed exactly once");
        require(api.events == std::vector<std::string>{"init","shutdown"},"refusal never calls uninitialized getters");
        require(sink.count("\"event\":\"refused\"") == 1 && sink.count("\"code\":121") == 1,"bounded numeric refusal recorded");
        require(sink.lines.size() == 1 && sink.count("\"event\":\"stopped\"") == 0,"refused is terminal without an unready stopped record");
    }
    {
        Fake api; Captured sink; Output out(sink);
        { Monitor monitor(api,out); require(monitor.start(true),"scope cleanup starts"); monitor.tick(); }
        require(api.count("shutdown") == 1 && api.events.back() == "shutdown","destructor ends owned client after final read");
    }
    {
        Fake api; Captured sink; sink.fail = true; Output out(sink); Monitor monitor(api,out);
        require(!monitor.start(true),"startup output failure refuses observer");
        require(api.count("shutdown") == 1,"output refusal still releases owned client");
    }
    {
        Fake api; Captured sink; Output out(sink); Monitor monitor(api,out); require(monitor.start(true),"output-loss setup");
        sink.fail = true; monitor.tick(); const auto events = api.events.size(); monitor.tick();
        require(api.events.size() == events && monitor.idle(),"broken output stops reads without retry loop");
    }
    {
        Fake api; Captured sink; Output out(sink); Monitor monitor(api,out); require(monitor.start(true),"record budget starts");
        for (unsigned i = 0; i < MaxSamples; ++i) { api.focus = i; monitor.tick(); }
        require(monitor.samples() == MaxSamples && monitor.idle() && monitor.active(),"32 snapshots enter idle, do not terminate client");
        const auto events = api.events.size(), lines = sink.lines.size();
        for (unsigned i = 0; i < 100; ++i) monitor.tick();
        require(api.events.size() == events && sink.lines.size() == lines,"spent budget makes no further API or output calls");
        require(api.count("shutdown") == 0,"budget is not an automatic lifetime timeout");
        monitor.stop(); require(api.count("shutdown") == 1,"budget-idle client still has explicit manual cleanup");
        require(sink.count("\"event\":\"budget\"") == 1 && out.bytes() <= MaxOutputBytes,"one budget receipt and bounded total output");
        for (const auto& line : sink.lines) require(line.size() <= MaxLineBytes,"every actual JSONL line fits the negotiated cap");
    }
    {
        Fake api; Captured sink; Output out(sink);
        const std::string padding(1000,'x');
        while (out.bytes() + 1024 < MaxOutputBytes - TerminalReserve) require(out.emit(padding+"\n"),"fill synthetic prior-output budget");
        Monitor monitor(api,out); require(monitor.start(true),"byte budget starts with remaining terminal room"); monitor.tick();
        require(monitor.idle() && monitor.active() && out.bytes() <= MaxOutputBytes,"byte budget idles without overflow");
        monitor.stop(); require(out.bytes() <= MaxOutputBytes && !out.failed(),"terminal record fits reserved output room");
    }
    {
        Fake api; Captured sink; Output out(sink); Monitor monitor(api,out); require(monitor.start(true),"numeric cases start");
        api.fg.r = std::numeric_limits<float>::quiet_NaN(); api.bg.g = std::numeric_limits<float>::infinity();
        api.grid = -std::numeric_limits<float>::infinity(); api.dimming.value = std::numeric_limits<float>::quiet_NaN(); monitor.tick();
        require(sink.lines.back().find("\"numeric_valid\":false") != std::string::npos,"nonfinite values explicitly unavailable");
        require(sink.lines.back().find("nan") == std::string::npos && sink.lines.back().find("inf") == std::string::npos,"no non-JSON NaN or infinity output");
        monitor.tick(); require(monitor.samples() == 1,"nonfinite unavailable state also deduplicates");
        api.fg = {}; api.bg = {}; api.grid = 0; api.dimming = {1,0}; monitor.tick();
        require(monitor.samples() == 2 && sink.lines.back().find("\"numeric_valid\":true") != std::string::npos,"valid values recover after numeric refusal");
        api.dimming = {std::numeric_limits<float>::quiet_NaN(),5}; api.noGrid = {true,5}; monitor.tick();
        require(sink.lines.back().find("\"scene_dimming_error\":5") != std::string::npos
            && sink.lines.back().find("\"do_not_fade_to_grid\":false") != std::string::npos,"absent settings retain API error without inventing a usable value");
        require(sink.lines.back().find("\"numeric_valid\":true") != std::string::npos,"unavailable float settings do not expose garbage as numeric input");
    }
    {
        Fake api; Captured sink; Output out(sink); Monitor monitor(api,out); require(monitor.start(true),"wide values start");
        const float large = std::numeric_limits<float>::max(); api.fg = {large,-large,large,-large}; api.bg = api.fg;
        api.grid = -large; api.dimming.value = large; api.focus = api.renderer = 0xffffffffu;
        api.noGrid.error = api.trackingFade.error = api.standalone.error = std::numeric_limits<std::int32_t>::min();
        api.hmd.deviceClass = std::numeric_limits<std::int32_t>::min(); api.hmd.model.error = std::numeric_limits<std::int32_t>::min();
        api.time = 0xfffffffffffffff0ull; monitor.tick();
        require(monitor.samples() == 1 && sink.lines.back().size() <= MaxLineBytes,"actual extreme scalar JSON stays within line bound");
    }
    {
        Fake api; Captured sink; Output out(sink); Monitor monitor(api,out); require(monitor.start(true),"identity transition starts");
        api.switching = true; monitor.tick();
        require(sink.lines.back().find("\"identity_stable\":false") != std::string::npos
            && sink.lines.back().find("\"frame_link_profile\":0") != std::string::npos,"mid-read identity switch never claims Frame Link");
        require(sink.lines.back().find("Deckard") == std::string::npos && sink.lines.back().find("Other HMD") == std::string::npos,"no device-supplied text or serial leaves observer");
    }
    Identity known{true,1,{"vrlink",0,true},{"Steam Frame",0,true},{"vrlink/frame",0,true}};
    require(profile(known,true) == 1,"canonical Frame Link identity"); known.model.value = "sTeAm FrAmE";
    require(profile(known,true) == 1,"ASCII case-insensitive exact identity");
    require(profile(known,false) == 0,"no profile from unstable identity"); known.connected = false;
    require(profile(known,true) == 0,"no profile from disconnected HMD"); known.connected = true; known.deviceClass = 2;
    require(profile(known,true) == 0,"no profile from non-HMD class"); known.deviceClass = 1;
    for (const auto& bad : {std::string("Steam Frame Extended"),std::string(97,'x'),std::string("Steam\nFrame")}) {
        known.model.value = bad; require(profile(known,true) == 0,"wrong, unbounded or control-containing model refused");
    }
    known.model = {"Deckard MP",0,true}; known.tracking = {"cv",0,true}; known.registration = {"",4,false};
    require(profile(known,true) == 2,"observed profile permits explicit absent registration");
    known.registration = {"virtualdesktop/123",0,true}; require(profile(known,true) == 0,"observed profile rejects contradictory transport");
    known.registration = {"vrlink/frame",0,true}; known.model.valid = false;
    require(profile(known,true) == 0,"invalid native string provenance cannot be classified");
    std::printf("compositor state monitor: %u checks passed; no Windows/OpenVR/runtime execution\n",checks);
}
