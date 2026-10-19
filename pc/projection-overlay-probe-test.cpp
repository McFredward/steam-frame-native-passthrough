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
    unsigned stage=0, failAt=0; bool failCleanup=false, zeroHandle=false;
    std::vector<std::string> events;
    bool step(const std::string& e) { events.push_back(e); return ++stage!=failAt; }
    bool initialize() override { return step("initialize"); }
    bool create(Handle& out) override { out=zeroHandle?0:11; return step("create"); }
    bool configure(Handle) override { return step("configure"); }
    bool texture(Handle) override { return step("texture"); }
    bool flush() override { return step("flush"); }
    bool submit(Handle) override { return step("submit"); }
    bool show(Handle) override { return step("show"); }
    bool reveal(Handle) override { return step("reveal"); }
    bool verify(Handle) override { return step("verify"); }
    bool ready() override { return step("ready"); }
    void stopped(bool clean) override { events.push_back(clean?"stopped":"cleanup-failed"); }
    bool cleanup(const char* e) { events.push_back(e); return !failCleanup; }
    bool hide(Handle) override { return cleanup("hide"); }
    bool clear(Handle) override { return cleanup("clear"); }
    bool destroy(Handle) override { return cleanup("destroy"); }
    void shutdown() override { events.push_back("shutdown"); }
    unsigned count(const std::string& e) const { return static_cast<unsigned>(std::count(events.begin(),events.end(),e)); }
    std::size_t index(const std::string& e) const { return static_cast<std::size_t>(std::find(events.begin(),events.end(),e)-events.begin()); }
};
int main() {
    {
        Fake f; Session s(f);
        require(!s.start(false)&&f.events.empty(),"opt-out makes no API call");
        require(s.start(true)&&s.active(),"explicit single mono overlay starts");
        require(!s.start(true)&&f.count("create")==1,"one owned overlay never duplicated");
        require(f.index("texture")<f.index("flush")&&f.index("flush")<f.index("submit"),"producer flush precedes compositor texture handoff");
        require(f.index("submit")<f.index("show")&&f.index("show")<f.index("reveal"),"complete mono texture prepared before visibility");
        require(f.index("reveal")<f.index("verify")&&f.index("verify")<f.index("ready"),"ready waits for local state checks");
        require(s.stop()&&!s.active(),"manual stop removes mono overlay");
        require(f.index("hide")<f.index("clear")&&f.index("clear")<f.index("destroy"),"hide clear destroy order");
        require(f.index("destroy")<f.index("shutdown")&&f.index("shutdown")<f.index("stopped"),"clean stopped waits for complete resource release");
        const auto n=f.events.size(); require(s.stop()&&f.events.size()==n,"manual stop idempotent");
        require(!s.start(true),"completed client cannot silently restart");
    }
    for (unsigned failure=1; failure<=10; ++failure) {
        Fake f; f.failAt=failure;
        { Session s(f); require(!s.start(true)&&!s.active(),"every startup stage can refuse");
          require(f.count("shutdown")==1,"partial init always cleaned immediately");
          require(f.count("destroy")==(failure>=2?1u:0u),"returned partial handle reclaimed exactly once");
          require(f.count("stopped")==0,"refusal never emits ready clean stopped"); }
        require(f.count("shutdown")==1,"destructor never repeats completed rollback");
    }
    {
        Fake f; f.zeroHandle=true; Session s(f);
        require(!s.start(true)&&f.count("configure")==0&&f.count("destroy")==0,"invalid handle refused before native use");
    }
    {
        Fake f; f.failCleanup=true;
        { Session s(f); require(s.start(true),"cleanup-failure setup"); require(!s.stop(),"cleanup failure reported"); }
        require(f.count("hide")==1&&f.count("clear")==1&&f.count("destroy")==1,"all cleanup attempted despite errors");
        require(f.count("shutdown")==1&&f.count("stopped")==0&&f.count("cleanup-failed")==1,"failed cleanup never reports clean end");
    }
    {
        Fake f; { Session s(f); require(s.start(true),"RAII setup"); }
        require(f.count("destroy")==1&&f.count("shutdown")==1,"scope exit ends only owned mono overlay");
    }
    require(PatternSize == 2048,"explicit 2048-square static input");
    require(pattern(2).empty(),"invalid eye rejected");
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto p = pattern(eye);
        require(p.size() == PatternSize*PatternSize,"bounded pattern extent");
        const auto at = [&](unsigned x,unsigned y) { return p[y*PatternSize+x]; };
        require(at(1024,1024).a==255&&at(1024,1024).r==192&&at(1024,1024).g==192&&at(1024,1024).b==192,"common center has a binocular opaque neutral witness");
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
    Matrix forward{}; forward.m[0][0]=forward.m[1][1]=forward.m[2][2]=1;
    const auto common=quadPose(forward);
    require(common.m[0][3]==0&&common.m[1][3]==0&&common.m[2][3]==-QuadDistance,"common plane centered on positive viewing ray");
    for (float eyeX : {-.032f,.032f}) {
        const float convergingRayX=-eyeX/QuadDistance;
        const float hitX=eyeX+QuadDistance*convergingRayX;
        require(std::abs(hitX)<1e-6f,"both eyes converge on same physical plane center");
        const float parallelU=.5f+eyeX/QuadWidth;
        require(parallelU>.125f&&parallelU<.875f,"both straight viewing rays hit visible interior instead of transparent edge");
    }
    const auto mono=pattern(0);
    require(mono.size()==PatternSize*PatternSize,"ordinary mono texture is square without stereo aspect interpretation");
    require(mono[1024*PatternSize+1024].a==255,"straight viewing axis intersects opaque neutral center witness");
    Matrix rotated{};rotated.m[0][0]=-1;rotated.m[1][1]=1;rotated.m[2][2]=-1;
    rotated.m[0][3]=2;rotated.m[1][3]=3;rotated.m[2][3]=4;
    const auto rotatedPlane=quadPose(rotated);
    require(rotatedPlane.m[0][3]==2&&rotatedPlane.m[1][3]==3&&std::abs(rotatedPlane.m[2][3]-5.2f)<1e-6f,"rotated translated head places common quad on its actual forward axis");
    std::printf("projection overlay lifecycle/pattern: %u checks passed; no Windows or Steam Link execution\n",checks);
}
