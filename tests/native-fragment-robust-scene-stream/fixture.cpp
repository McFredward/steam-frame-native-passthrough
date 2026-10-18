// Full decoded images, never repainted code vectors. The private original and
// candidate are explicit bounded inputs; this fixture is not a receiver hook.
#include <vulkan/vulkan.h>
static VkResult linearSampler(VkDevice d,const VkSamplerCreateInfo* in,const VkAllocationCallbacks* a,VkSampler* out){
    auto info=*in;info.minFilter=info.magFilter=VK_FILTER_LINEAR;return vkCreateSampler(d,&info,a,out);
}
#define vkCreateSampler linearSampler
#include "borrowed_guard_fixture.h"
#undef vkCreateSampler
namespace {
struct StreamCase { Case native;Vec expected,source,q,span;std::uint32_t role,valid; };
static_assert(sizeof(StreamCase)==160,"bounded stream oracle record");
std::vector<StreamCase> cases(const char* path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"explicit cases open");const auto size=f.tellg();
    require(size>=12&&size<=12+128*160,"bounded case file");f.seekg(0);char magic[8]{};unsigned count{};
    require(bool(f.read(magic,8))&&std::memcmp(magic,"SFNPSTR1",8)==0,"case schema");
    require(bool(f.read(reinterpret_cast<char*>(&count),4))&&count&&count<=128,"case count");
    require(size==std::streamoff(12+count*160),"complete case extent");std::vector<StreamCase> out(count);
    require(bool(f.read(reinterpret_cast<char*>(out.data()),count*160)),"complete cases");
    for(const auto& c:out){require(c.native.eye<2&&c.native.flags==0x02038018u&&c.role<5&&c.valid==1,"explicit selected original-metadata route");
        for(const auto* v:{&c.expected,&c.source,&c.q,&c.span,&c.native.u,&c.native.r0,&c.native.r1,&c.native.patch,&c.native.tint})
            for(float x:*v)require(std::isfinite(x),"finite case fields");}
    return out;
}
std::vector<float> pixels(const char* path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"full decoded texture opens");
    require(f.tellg()==std::streamoff(SourceWidth)*SourceHeight*16,"exact selected texture extent");f.seekg(0);
    std::vector<float> out(std::size_t(SourceWidth)*SourceHeight*4);require(bool(f.read(reinterpret_cast<char*>(out.data()),out.size()*4)),"full decoded texture read");
    require(std::all_of(out.begin(),out.end(),[](float x){return std::isfinite(x);}),"finite decoded texture");return out;
}
void jsonVec(const Vec& v){std::cout<<'[';for(unsigned i=0;i<4;++i){if(i)std::cout<<',';std::cout<<v[i];}std::cout<<']';}
Vec floats(const std::array<std::uint32_t,4>& a){return {floating(a[0]),floating(a[1]),floating(a[2]),floating(a[3])};}
}
int main(int argc,char** argv){
    try{
        require(argc==6,"VERTEX ORIGINAL CANDIDATE FULL_PACKED_FLOAT_TEXTURE CASES");
        auto records=cases(argv[5]);auto decoded=pixels(argv[4]);GuardGpu gpu;gpu.init();require(gpu.validationEnabled,"active Khronos validation required");
        gpu.guardResources();gpu.guardPipelines(argv[1],argv[2],argv[3],true);gpu.update(0,decoded);
        std::array<bool,2> eyeGood{{true,true}};std::array<unsigned,2> fallbacks{},roles{},samples{};bool good=true;
        float maximumOracle=0,maximumSource=0;unsigned emission=0;std::cout<<"{\"rows\":[";
        for(unsigned index=0;index<records.size();++index){const auto& c=records[index];const auto eye=c.native.eye;
            gpu.metadata(c.native);gpu.record(c.native.u);
            const auto old=gpu.execute(0,0).rgba,out=gpu.execute(0,1).rgba,restored=gpu.execute(0,0).rgba;
            require(old==restored,"unmodified original output restores bit-exactly");const auto observed=floats(out);const auto over=floats(gpu.execute(0,2).rgba);
            bool numeric=true,source=true;
            for(unsigned i=0;i<4;++i){const float tolerance=.0001f+c.span[i]*std::ldexp(2.f,-int(gpu.properties.limits.subTexelPrecisionBits));
                const float e=std::abs(observed[i]-c.expected[i]);maximumOracle=std::max(maximumOracle,e);numeric&=std::isfinite(observed[i])&&e<=tolerance;
                const float originalError=std::abs(observed[i]-c.source[i]);maximumSource=std::max(maximumSource,originalError);source&=originalError<=4.f/255;
                const float expectedOver=c.expected[i]+Background[i]*(1-c.expected[3]);
                const float overTolerance=tolerance+Background[i]*(.0001f+c.span[3]*std::ldexp(2.f,-int(gpu.properties.limits.subTexelPrecisionBits)));
                numeric&=std::isfinite(over[i])&&std::abs(over[i]-expectedOver)<=overTolerance;
            }
            // Original fallback is an error for every advertised atlas stream.
            // A matching ordinary pixel alone cannot prove a guard decision;
            // partial/emission witnesses make each whole-eye criterion decisive.
            const bool fallback=out==old;if(fallback)++fallbacks[eye];eyeGood[eye]=eyeGood[eye]&&numeric&&source;
            good&=numeric&&source;roles[eye]|=1u<<c.role;++samples[eye];
            if(c.role==4){++emission;good&=observed[3]<.02f&&(observed[0]+observed[1]+observed[2])>.05f;}
            if(index){std::cout<<',';}
            std::cout<<"{\"eye_selector\":"<<eye<<",\"role\":"<<c.role<<",\"q\":";jsonVec(c.q);
            std::cout<<",\"output\":";jsonVec(observed);std::cout<<",\"expected_decoded_panels\":";jsonVec(c.expected);
            std::cout<<",\"source_before_codec\":";jsonVec(c.source);std::cout<<",\"original\":";jsonVec(floats(old));
            std::cout<<",\"panel_oracle_match\":"<<(numeric?"true":"false")<<",\"source_quality_match\":"<<(source?"true":"false")<<",\"exact_original_output\":"<<(fallback?"true":"false")<<'}';
        }
        require(samples[0]&&samples[1]&&roles[0]==31&&roles[1]==31&&emission>=2,"both actual source eyes contain independent glass and additive witnesses");
        // Ordinary-image OFF/recovery uses unchanged already-recorded commands.
        // Texture storage alone changes; no CPU classification or new pipeline.
        std::vector<float> ordinary(decoded.size());const auto solid=encodeColor(Vec{.3f,.4f,.6f,1},0x02038018u);
        for(std::size_t i=0;i<ordinary.size();i+=4)std::copy(solid.begin(),solid.end(),ordinary.begin()+i);
        unsigned ordinaryControls=0;
        for(unsigned eye=0;eye<2;++eye){const auto p=std::find_if(records.begin(),records.end(),[&](const StreamCase& c){return c.native.eye==eye&&c.role==4;});
            require(p!=records.end(),"per-eye emission witness");gpu.metadata(p->native);gpu.record(p->native.u);const auto before=gpu.execute(0,1).rgba;const auto generation=gpu.recordings;
            gpu.update(0,ordinary);const auto old=gpu.execute(0,0).rgba,out=gpu.execute(0,1).rgba;require(old==out,"ordinary source must keep exact original994");
            gpu.update(0,decoded);const auto after=gpu.execute(0,1).rgba;require(before==after&&gpu.recordings==generation,"same-CB ordinary/OFF then original atlas recovery");++ordinaryControls;
        }
        require(validationErrors.load()==0,"no Vulkan/sync error");
        std::cout<<"],\"stereo_criterion_pass\":"<<(good?"true":"false")<<",\"eye_matches\":["<<(eyeGood[0]?"true":"false")<<','<<(eyeGood[1]?"true":"false")
            <<"],\"exact_original_pixels\":["<<fallbacks[0]<<','<<fallbacks[1]<<"],\"source_extent\":["<<SourceWidth<<','<<SourceHeight<<"],\"ordinary_exact_controls\":"<<ordinaryControls
            <<",\"max_decoded_oracle_error\":"<<maximumOracle<<",\"max_source_error\":"<<maximumSource<<",\"sub_texel_precision_bits\":"<<gpu.properties.limits.subTexelPrecisionBits
            <<",\"validation_errors\":"<<validationErrors.load()<<",\"checks\":"<<checks<<",\"physical_eye_association_proven\":false}\n";
        return good?0:2;
    }catch(const std::exception& e){std::cerr<<"stream fixture: "<<e.what()<<"; checks="<<checks<<'\n';return 1;}
}
