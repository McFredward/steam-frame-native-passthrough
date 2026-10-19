#include "helper_fixture.h"
#include "receiver/stream_bridge_camera_client.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
namespace {
unsigned checks=0;
void check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
struct Root {
    std::filesystem::path path;
    Root(const char* mode="normal"){
        char name[]="/tmp/sfnp-camera-client-XXXXXX";check(mkdtemp(name),"private helper fixture root");path=name;
        sfnp_camera_fixture::write(path,mode);
    }
    ~Root(){std::error_code error;std::filesystem::remove_all(path,error);}
    std::string events()const{std::ifstream in(path/"events");return {std::istreambuf_iterator<char>(in),{}};}
};
template<class Condition>void until(stream_bridge_camera::Client& client,const Root& root,bool peer,Condition condition,const char* text){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()&&std::chrono::steady_clock::now()<end){client.tickCold(root.path,peer);std::this_thread::yield();}
    check(condition(),text);
}
void stop(stream_bridge_camera::Client& client,const Root& root){client.stopCold();until(client,root,false,[&]{return !client.runningCold();},"owned helper reaped after EOF without signaling a native process");check(root.events().find("eof-restored")!=std::string::npos,"real subprocess received EOF and performed its final restoration");}
void lifecycle(){
    Root root;stream_bridge_camera::Client client;
    for(unsigned n=0;n<20;++n)client.tickCold(root.path,false);
    check(!client.runningCold()&&!std::filesystem::exists(root.path/"events"),"no verified stream means no subprocess or startup setting action");
    until(client,root,true,[&]{return root.events().find("D ")!=std::string::npos;},"real connected stream prepares Background helper in disabled state");
    check(!client.ready()&&root.events().find("E ")==std::string::npos,"warm peer never requests native alpha on its own");
    client.request(true);check(!client.ready(),"enable request alone cannot admit rendering before API acknowledgment");
    until(client,root,true,[&]{return client.ready();},"matching real subprocess enable acknowledgment admits camera alpha");
    client.request(false);check(!client.ready(),"None revokes readiness atomically before sending restore request");
    until(client,root,true,[&]{const auto text=root.events();return text.find("E ")!=std::string::npos&&text.rfind("restored")>text.find("E ");},"disabled session restores native baseline through independent helper");
    client.request(true);until(client,root,true,[&]{return client.ready();},"same session can enable again after explicit None without a timer");
    client.tickCold(root.path,false);check(!client.ready(),"peer loss withdraws render permission immediately");
    until(client,root,false,[&]{return !client.runningCold();},"disconnect receives restoration acknowledgment and closes owned helper");
    check(root.events().find("eof-restored")!=std::string::npos,"disconnect also preserves EOF restoration contract");
}
void stale(){
    Root root("hold");stream_bridge_camera::Client client;client.request(true);
    until(client,root,true,[&]{return root.events().find("E ")!=std::string::npos;},"enable request reaches helper but its acknowledgment is intentionally withheld");
    check(!client.ready(),"unacknowledged enable remains unavailable");client.request(false);
    until(client,root,true,[&]{return root.events().find("stale-sent")!=std::string::npos;},"disable overtakes stale enable using greater sequence");
    for(unsigned n=0;n<128;++n){client.tickCold(root.path,true);check(!client.ready(),"stale enable acknowledgment never revives explicitly disabled session");}
    std::ofstream(root.path/"release")<<"ready";
    stop(client,root);
}
void refusal(){
    {Root root("bad");stream_bridge_camera::Client client;client.request(true);
     until(client,root,true,[&]{return root.events().find("eof-restored")!=std::string::npos;},"invalid helper response closes only private channel and triggers restore");
     check(!client.ready(),"invalid response never creates camera authority");stop(client,root);}
    {Root root;chmod((root.path/"camera-session.json").c_str(),0644);stream_bridge_camera::Client client;client.request(true);client.tickCold(root.path,true);check(!client.runningCold()&&!client.ready()&&!std::filesystem::exists(root.path/"events"),"nonprivate configuration is refused before helper execution");}
    {Root root;std::filesystem::rename(root.path/"tools/stream_bridge_camera.py",root.path/"owned.py");std::filesystem::create_symlink(root.path/"owned.py",root.path/"tools/stream_bridge_camera.py");stream_bridge_camera::Client client;client.request(true);client.tickCold(root.path,true);check(!client.runningCold()&&!client.ready()&&!std::filesystem::exists(root.path/"events"),"symlink helper cannot acquire runtime settings authority");}
}
struct Clock {
    stream_bridge_camera::ClockSample sample{12000000000ULL,10000000000ULL};
    static bool read(stream_bridge_camera::ClockSample& out,void* p) noexcept{out=static_cast<Clock*>(p)->sample;return true;}
};
unsigned enables(const Root& root){unsigned count=0;std::istringstream lines(root.events());std::string line;while(std::getline(lines,line))if(line.rfind("E ",0)==0)++count;return count;}
void resume(){
    Root root("resume");Clock clock;stream_bridge_camera::Client client(Clock::read,&clock);client.request(true);
    until(client,root,true,[&]{return client.ready();},"unchanged active source initially has an acknowledged camera session");
    const auto initial=root.events();clock.sample.bootNanoseconds+=7000000000ULL;clock.sample.monotonicNanoseconds+=7000000000ULL;client.tickCold(root.path,true);
    check(client.ready()&&root.events()==initial,"pure scheduling stall advances both clocks and never renews settings");
    clock.sample.monotonicNanoseconds+=20000000ULL;client.tickCold(root.path,true);
    clock.sample.bootNanoseconds+=20000000ULL;client.tickCold(root.path,true);
    check(client.ready()&&root.events()==initial,"preemption between native clock reads never lowers resume baseline or invents renewal");
    clock.sample.bootNanoseconds+=4000000ULL;client.tickCold(root.path,true);check(client.ready(),"sub-threshold sampling jitter does not invent a suspend");
    clock.sample.bootNanoseconds+=3000000000ULL;client.tickCold(root.path,true);check(!client.ready(),"real suspend offset revokes old ready acknowledgment immediately despite unchanged MR demand");
    until(client,root,true,[&]{return root.events().find("restore-pending")!=std::string::npos;},"resume requests explicit baseline restore before renewal");
    for(unsigned n=0;n<128;++n){client.tickCold(root.path,true);check(!client.ready()&&enables(root)==1,"resume cannot re-enable before matching restore acknowledgment or adopt stale previous A");}
    std::ofstream(root.path/"restore-release")<<"ready";
    until(client,root,true,[&]{return client.ready();},"fresh E/A only after acknowledged D restores active unchanged stream");
    check(enables(root)==2,"resume sends exactly one new enable after baseline restoration");stop(client,root);
}
void health(){
    Root root("health");stream_bridge_camera::Client client;client.request(true);
    until(client,root,true,[&]{return client.ready();},"native health fixture first has a current acknowledged session");
    std::ofstream(root.path/"health-loss")<<"runtime changed";
    until(client,root,true,[&]{return !client.ready();},"explicit native API/session loss N revokes prior acknowledged readiness");
    stop(client,root);
}
}
int main(int argc,char** argv){try{if(argc==2&&std::string(argv[1])=="resume")resume();else{lifecycle();stale();refusal();resume();health();}std::printf("%u actual camera client subprocess checks passed\n",checks);return 0;}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
