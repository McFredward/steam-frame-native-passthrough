#include "projection-overlay-probe-core.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace projection_probe;
unsigned checks = 0;
void require(bool condition,const char* what) {
    ++checks;
    if (!condition) { std::fprintf(stderr,"FAIL %s\n",what); std::exit(1); }
}
struct Fake final : Api {
    unsigned stage = 0, failAt = 0;
    bool failCleanup = false, duplicate = false;
    std::vector<std::string> events;
    bool step(const std::string& event) { events.push_back(event); return ++stage != failAt; }
    bool initialize() override { return step("initialize"); }
    bool create(unsigned eye,Handle& out) override {
        out = duplicate ? 11 : 11 + eye;
        return step("create"+std::to_string(eye));
    }
    bool configure(unsigned eye,Handle) override { return step("configure"+std::to_string(eye)); }
    bool texture(unsigned eye,Handle) override { return step("texture"+std::to_string(eye)); }
    bool show(Handle h) override { return step("show"+std::to_string(h)); }
    bool reveal(Handle h) override { return step("reveal"+std::to_string(h)); }
    bool ready() override { return step("ready"); }
    void stopped(bool clean) override { events.push_back(clean ? "stopped" : "cleanup-failed"); }
    bool cleanup(const char* name,Handle h) {
        events.push_back(std::string(name)+std::to_string(h)); return !failCleanup;
    }
    bool hide(Handle h) override { return cleanup("hide",h); }
    bool clear(Handle h) override { return cleanup("clear",h); }
    bool destroy(Handle h) override { return cleanup("destroy",h); }
    void shutdown() override { events.push_back("shutdown"); }
    unsigned count(const std::string& event) const {
        return static_cast<unsigned>(std::count(events.begin(),events.end(),event));
    }
    std::size_t index(const std::string& event) const {
        return static_cast<std::size_t>(std::find(events.begin(),events.end(),event)-events.begin());
    }
};
int main() {
    {
        Fake f; Session s(f);
        require(!s.start(false),"no implicit opt-in");
        require(f.events.empty(),"opt-out makes no API call");
        require(s.start(true),"explicit start");
        require(s.active(),"both eyes active");
        require(!s.start(true),"duplicate active start refused");
        require(f.index("texture1") < f.index("show11"),"all eye textures prepared before publication");
        require(f.index("configure1") < f.index("texture0"),"both frusta prepared before texture submission");
        require(s.stop(),"manual stop");
        require(!s.active(),"no active eye after stop");
        require(f.index("hide12") < f.index("clear11"),"hide both before clearing");
        require(f.index("clear12") < f.index("destroy11"),"clear both before destroying");
        require(f.index("destroy12") < f.index("shutdown"),"destroy before API/device lifetime ends");
        require(f.index("show12") < f.index("reveal11"),"both alpha-zero eyes shown before revealing either");
        require(f.index("reveal12") < f.index("ready"),"ready waits for both alpha-one reveals");
        require(f.index("shutdown") < f.index("stopped"),"stopped waits for clean resource release");
        const auto count = f.events.size();
        require(s.stop() && f.events.size() == count,"manual stop idempotent");
        require(!s.start(true),"completed client cannot silently restart");
    }
    for (unsigned failure = 1; failure <= 12; ++failure) {
        Fake f; f.failAt = failure;
        {
            Session s(f);
            require(!s.start(true),"each startup stage can refuse");
            require(!s.active(),"partial startup never reported active");
            require(f.count("shutdown") == 1,"partial initialization always shuts down once");
            if (failure >= 2) require(f.count("destroy11") == 1,"partial create handle reclaimed");
            if (failure >= 3) require(f.count("destroy12") == 1,"right handle reclaimed even on failure");
            require(f.index("shutdown") == f.events.size()-1,"shutdown last on refusal");
            require(f.count("stopped") == 0,"refusal never reports a ready clean stop");
        }
        require(f.count("shutdown") == 1,"destructor does not repeat completed rollback");
    }
    {
        Fake f; f.duplicate = true; Session s(f);
        require(!s.start(true),"duplicate overlay handles refused");
        require(f.count("destroy11") == 1,"aliased handle destroyed once");
    }
    {
        Fake f; f.failCleanup = true;
        { Session s(f); require(s.start(true),"cleanup-failure setup");
          require(!s.stop(),"cleanup failure reported"); }
        require(f.count("hide11") == 1 && f.count("hide12") == 1,"cleanup attempts both hides");
        require(f.count("clear11") == 1 && f.count("clear12") == 1,"cleanup attempts both clears");
        require(f.count("destroy11") == 1 && f.count("destroy12") == 1,"cleanup attempts both destroys");
        require(f.count("shutdown") == 1,"cleanup errors still release API lifetime");
    }
    {
        Fake f; { Session s(f); require(s.start(true),"destructor cleanup setup"); }
        require(f.count("destroy11") == 1 && f.count("destroy12") == 1,"scope exit reclaims both eyes");
    }
    require(PatternSize == 2048,"explicit 2048-square static input");
    require(pattern(2).empty(),"invalid eye rejected");
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto p = pattern(eye);
        require(p.size() == PatternSize*PatternSize,"bounded pattern extent");
        const auto at = [&](unsigned x,unsigned y) { return p[y*PatternSize+x]; };
        require(at(0,0).a == 0 && at(0,0).r == 0 && at(0,0).g == 0 && at(0,0).b == 0,"transparent border");
        constexpr std::array<unsigned,8> alpha{{0,64,128,255,64,128,192,255}};
        for (unsigned band = 0; band < 8; ++band) {
            const auto v = at(256 + 192*band + 96,800);
            require(v.a == alpha[band],"known INNER independent-alpha lane");
            if (band < 4) require(v.r == 0 && v.g == 0 && v.b == 0,"identical RGB black includes transparent and opaque interior witnesses");
            if (band == 4 || band == 5) require(v.r == v.a/2 && v.g == v.r && v.b == v.r,"fractional neutral-grey witness");
            if (band >= 6) require(v.g == v.a && (eye == 0 ? v.r == v.a && v.b == 0 : v.b == v.a && v.r == 0),"per-eye colored witness");
        }
        bool premultiplied = true;
        for (auto v : p) premultiplied &= v.r <= v.a && v.g <= v.a && v.b <= v.a;
        require(premultiplied,"all RGB valid premultiplied contributions");
        require(at(257,1610).r == 255 && at(256,1610).r == 0 && at(256,1610).a == 255,"real one-pixel black-white detail bars");
        for (unsigned band = 0; band < 7; ++band) {
            const unsigned period = 1u << band, y = 1624 + band * 48;
            require(at(256,y).r == 0 && at(256,y).a == 255 && at(256+period,y).r == 255,
                "distinct 1/2/4/8/16/32/64-texel detail periods");
        }
        require(at(256,1935).a == 255 && at(256,1936).a == 0,"seven detail bands stay inside transparent border");
        require(at(322,150).a == (eye == 1 ? 255 : 0),"different interior eye IDs");
    }
    Frustum full{-1.2f,.9f,-1.1f,1.3f}, cropped{};
    require(central(full,cropped),"valid asymmetric full frustum accepted");
    require(std::abs((cropped.left-full.left)/(full.right-full.left)-.40f)<1e-6f
        && std::abs((cropped.right-full.left)/(full.right-full.left)-.60f)<1e-6f,"horizontal placement uses original chart center");
    require(std::abs((cropped.top-full.top)/(full.bottom-full.top)-.40f)<1e-6f
        && std::abs((cropped.bottom-full.top)/(full.bottom-full.top)-.60f)<1e-6f,"vertical placement uses original chart center");
    require(ChartMin > 560.f/3168 && ChartMax < 2608.f/3168,"entire overlay stays strictly inside v3 RGB tile, away from mask and guards");
    full.right=full.left; require(!central(full,cropped),"degenerate frustum refused");
    full.right=std::numeric_limits<float>::infinity(); require(!central(full,cropped),"nonfinite frustum refused");
    struct Capture final : LogSink { std::string text; bool failing = false;
        bool write(const std::string& line) override { if (failing) return false; text+=line; return true; }
    } sink;
    Log log(sink); require(log.emit(ReadyLine),"exact ready token emitted");
    require(sink.text == "SFNP_PROJECTION_READY_V2\n","ready has no incidental prefix or metadata");
    require(log.emit(StoppedLine),"exact stopped token emitted");
    require(!log.emit("untrusted\ttext\n"),"non-ASCII-control metadata refused");
    require(!log.emit(ReadyLine),"failed output remains refused");
    Capture capped; Log boundedLog(capped);
    for (unsigned i=0; i<8; ++i) require(boundedLog.emit(std::string(1023,'x')+"\n"),"bounded log chunk accepted");
    require(boundedLog.bytes()==LogCap && !boundedLog.emit(ReadyLine),"aggregate metadata never exceeds 8KiB");
    Capture broken; broken.failing=true; Log failedLog(broken);
    require(!failedLog.emit(ReadyLine) && failedLog.failed(),"broken pipe marks failed output");
    Matrix head{}; head.m[0][2] = 1; head.m[1][1] = 1; head.m[2][0] = -1;
    head.m[0][3] = 1; head.m[1][3] = 2; head.m[2][3] = 3;
    Matrix eye{}; eye.m[0][0] = eye.m[1][1] = eye.m[2][2] = 1; eye.m[0][3] = -.032f;
    const auto absolute = compose(head,eye);
    require(finite(absolute),"composed pose finite");
    require(absolute.m[0][3] == 1 && absolute.m[1][3] == 2 && std::abs(absolute.m[2][3]-3.032f) < 1e-6f,"eye offset rotates in head space");
    head.m[2][2] = std::numeric_limits<float>::quiet_NaN();
    require(!finite(head),"nonfinite pose refused");
    std::printf("projection overlay lifecycle/pattern: %u checks passed; no Windows or Steam Link execution\n",checks);
}
