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
        const auto count = f.events.size();
        require(s.stop() && f.events.size() == count,"manual stop idempotent");
    }
    for (unsigned failure = 1; failure <= 9; ++failure) {
        Fake f; f.failAt = failure;
        {
            Session s(f);
            require(!s.start(true),"each startup stage can refuse");
            require(!s.active(),"partial startup never reported active");
            require(f.count("shutdown") == 1,"partial initialization always shuts down once");
            if (failure >= 2) require(f.count("destroy11") == 1,"partial create handle reclaimed");
            if (failure >= 3) require(f.count("destroy12") == 1,"right handle reclaimed even on failure");
            require(f.index("shutdown") == f.events.size()-1,"shutdown last on refusal");
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
    require(pattern(2).empty(),"invalid eye rejected");
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto p = pattern(eye);
        require(p.size() == PatternSize*PatternSize,"bounded pattern extent");
        const auto at = [&](unsigned x,unsigned y) { return p[y*PatternSize+x]; };
        require(at(0,0).a == 0 && at(0,0).r == 0 && at(0,0).g == 0 && at(0,0).b == 0,"transparent border");
        require(at(400,340).a == 255 && at(400,340).r == 0 && at(400,340).g == 0 && at(400,340).b == 0,"opaque black must remain opaque");
        for (unsigned x : {80u,180u,280u,400u}) {
            const auto v = at(x,200);
            require(v.a == ((x-64)/96+1)*255/4,"known independent quarter-alpha bands");
            require(v.g == v.a && (eye == 0 ? v.r == v.a && v.b == 0 : v.b == v.a && v.r == 0),"unambiguous eye color");
        }
        bool premultiplied = true;
        for (auto v : p) premultiplied &= v.r <= v.a && v.g <= v.a && v.b <= v.a;
        require(premultiplied,"all RGB is valid premultiplied contribution");
        require(at(81,420).a == 255 && at(80,420).a == 63,"one pixel detail is real input, not an upscaled image");
        require(at(140,110).r == (eye == 1 ? 255 : 63),"distinct per-eye spatial ID");
    }
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
