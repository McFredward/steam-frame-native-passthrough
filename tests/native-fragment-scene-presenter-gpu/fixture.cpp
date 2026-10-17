// Owned current-image atlas proof. The private native fragment is an input,
// never embedded here. Reuse the existing owned allocation/metadata/CB helper.
#include <vulkan/vulkan.h>
static VkResult linearSampler(VkDevice d,const VkSamplerCreateInfo* in,const VkAllocationCallbacks* a,VkSampler* out){
    auto info=*in;info.minFilter=info.magFilter=VK_FILTER_LINEAR;return vkCreateSampler(d,&info,a,out);
}
#define vkCreateSampler linearSampler
#include "borrowed_guard_fixture.h"
#undef vkCreateSampler
namespace {
struct SceneCase {
    GuardCase base;
    Vec colorU,maskU;
    float colorAperture,maskAperture,originalAperture;
    std::array<float,3> inverseG;
    float colorWeight,maskWeight;
    std::uint32_t pattern;
};
static_assert(sizeof(SceneCase)==228,"fixed new owned scene case");
std::vector<SceneCase> readCases(const char* name){
    std::ifstream file(name,std::ios::binary|std::ios::ate);require(bool(file),"scene cases input");
    const auto bytes=file.tellg();require(bytes>=12&&bytes<=12+256*228,"bounded scene case bytes");file.seekg(0);
    char magic[8]{};unsigned count{};require(bool(file.read(magic,8))&&std::memcmp(magic,"SCENEAT1",8)==0,"scene case schema");
    require(bool(file.read(reinterpret_cast<char*>(&count),4))&&count&&count<=256,"bounded scene case count");
    require(bytes==std::streamoff(12+count*228),"complete scene case range");std::vector<SceneCase> out(count);
    require(bool(file.read(reinterpret_cast<char*>(out.data()),count*228)),"complete owned scene records");
    for(const auto& c:out)require((c.pattern&255)<8&&(c.pattern>>8)<2&&c.base.slot<2&&c.base.sourceChart<4&&c.base.native.eye<2&&c.base.cell<12,"bounded owned scene inputs");
    return out;
}
float maskValue(double x,double y,unsigned pattern,unsigned size){
    if(pattern==0)return 0;
    if(pattern==1)return 1;
    if(pattern==3)return -.2f;
    if(pattern==4)return 1.2f;
    if(pattern==6)return .25f;
    if(pattern==7)return .5f;
    x=std::max(0.,std::min(double(size-1),x));y=std::max(0.,std::min(double(size-1),y));
    if(pattern==5)return x<(size-1)*.5?0.f:1.f;
    return float(.17+.51*x/(size-1)+.21*y/(size-1));
}
std::vector<float> sceneSource(const SceneCase& record){
    const auto& c=record.base;const auto& chart=Charts[c.sourceChart];const Vec color=encodeColor(c.body,c.native.flags);
    std::vector<float> out(SourceWidth*SourceHeight*4);constexpr unsigned codes[12]{111,208,128,104,227,251,255,193,102,168,15,90};
    const bool full=(record.pattern>>8)==1;const unsigned pattern=record.pattern&255;
    const double legacy[4][2]{{16,48},{1072,48},{16,2448},{1072,2448}};
    const double windmill[4][2]{{576,32},{2624,576},{544,2624},{32,544}};
    const auto& origins=full?windmill:legacy;const unsigned size=full?2048:1024,rows=size/4;
    constexpr unsigned fullCodes[12]{104,14,149,250,167,249,185,25,121,144,38,152};
    const double canvasX=full?3168:2112,canvasY=full?3168:2752;
    for(unsigned y=0;y<SourceHeight;++y)for(unsigned x=0;x<SourceWidth;++x){
        const double ax=(((double(x)+.5)/SourceWidth-chart[0])/(chart[2]-chart[0]))*canvasX;
        const double ay=(((double(y)+.5)/SourceHeight-chart[1])/(chart[3]-chart[1]))*canvasY;
        Vec v{.24f,.53f,.69f,1};
        const double sx=full?560:32,sy=full?560:352;
        if(ax>=sx&&ax<sx+2048&&ay>=sy&&ay<sy+2048)v=color;
        for(unsigned tile=0;tile<4;++tile){const auto ox=origins[tile][0],oy=origins[tile][1];
            const bool rotate=full&&(tile%2==1);const auto w=rotate?rows:size,h=rotate?size:rows;
            if(ax>=ox-16&&ax<ox+w+16&&ay>=oy-16&&ay<oy+h+16){
                const double gx=rotate?size-(ay-oy)-.5:ax-ox-.5;
                const double gy=(rotate?ax-ox-.5:ay-oy-.5)+tile*rows;
                v={.31f,maskValue(gx,gy,pattern,size),.68f,1};break;
            }
        }
        if(c.mode==7)v={.21f,.5f,.72f,1};
        if(c.mode!=5&&c.mode!=7){double gx=ax,gy=ay;
            if(c.mode==2)gx-=80;
            if(c.mode==3)gx=2112-ax;
            if(c.mode==4){gx=64+(ax-64)*2;gy=2400+(ay-2400)*2;}
            for(unsigned cell=0;cell<12;++cell){const double ox=64+(full?36:160)*cell,oy=full?64+36*cell:2400;
              if(gx>=ox&&gx<ox+32&&gy>=oy&&gy<oy+32){
                int value=int(full?fullCodes[cell]:codes[cell]);if(c.mode==1)value^=0x5a;if(c.mode==8&&cell==c.cell)value+=c.delta;
                v={.31f,float(value)/255,.68f,1};break;
            }}
        }
        std::copy(v.begin(),v.end(),out.begin()+4*(y*SourceWidth+x));
    }return out;
}
bool sameSource(const SceneCase& a,const SceneCase& b){
    return a.pattern==b.pattern&&a.base.mode==b.base.mode&&a.base.cell==b.base.cell&&a.base.delta==b.base.delta&&
        a.base.sourceChart==b.base.sourceChart&&((a.base.native.flags^b.base.native.flags)&(4u<<16))==0&&a.base.body==b.base.body;
}
}
int main(int argc,char** argv){
    try{
        require(argc==6,"usage: scene-fixture VERTEX ORIGINAL CANDIDATE CASES enabled|off|coordinates");
        const bool coordinates=std::string(argv[5])=="coordinates";
        const bool enabled=coordinates||std::string(argv[5])=="enabled";require(enabled||std::string(argv[5])=="off","explicit scene mode");
        auto cases=readCases(argv[4]);GuardGpu gpu;gpu.init();require(gpu.validationEnabled,"Khronos validation required");
        gpu.guardResources();gpu.guardPipelines(argv[1],argv[2],argv[3],true);
        unsigned valid=0,invalid=0,additive=0,seams=0,unequal=0,colorOracles=0,maskOracles=0,selectors=0,branches=0;
        float maxError=0;Vec recorded{};bool hasRecord=false;unsigned firstGeneration=0,transitionGeneration=0;
        std::array<bool,2> hasImage{};std::array<SceneCase,2> previous{};
        std::vector<std::pair<unsigned,Vec>> observedImage;
        for(unsigned index=0;index<cases.size();++index){const auto& r=cases[index];const auto& c=r.base;
            if(!hasRecord||recorded!=c.native.u){gpu.record(c.native.u);recorded=c.native.u;hasRecord=true;}
            if(index==0)firstGeneration=gpu.recordings;
            if(cases.size()>160&&index==cases.size()-4)transitionGeneration=gpu.recordings;
            if(cases.size()>160&&index>=cases.size()-4)require(gpu.recordings==transitionGeneration,"v2 ordinary v1 v2 same existing command buffers");
            if(index<3)require(gpu.recordings==firstGeneration,"ON OFF ON same already-recorded command buffers");
            if(!hasImage[c.slot]||!sameSource(previous[c.slot],r)){gpu.update(c.slot,sceneSource(r));previous[c.slot]=r;hasImage[c.slot]=true;}
            gpu.metadata(c.native);const auto original=gpu.execute(c.slot,0).rgba;
            const auto output=gpu.execute(c.slot,1).rgba;const auto restored=gpu.execute(c.slot,0).rgba;
            require(original==restored,"original command restoration bit exact");
            // A discontinuous step is sensitive to the physical sampler
            // subtexel precision; smooth/constant inherited checks stay strict.
            const float stepTolerance=(r.pattern&255)==5?std::ldexp(1.f,-int(gpu.properties.limits.subTexelPrecisionBits))+.000025f:.000025f;
            auto nearSample=[&](float a,float b,const std::string& label,float tolerance){require(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance,label+" differs");};
            if(!enabled||!c.valid){require(output==original,"invalid/OFF must retain exact original994 words at case "+std::to_string(index));++invalid;continue;}
            observedImage.push_back({index,{floating(output[0]),floating(output[1]),floating(output[2]),floating(output[3])}});
            for(unsigned i=0;i<4;++i){const float observed=floating(output[i]);nearSample(observed,c.expected[i],(std::string(i==3?"scene coverage alpha":"scene contribution RGB")+" case "+std::to_string(index)+" observed "+std::to_string(observed)+" expected "+std::to_string(c.expected[i])),i==3?stepTolerance:.000025f);maxError=std::max(maxError,std::abs(observed-c.expected[i]));}
            if(coordinates){++valid;seams+=c.mode==9;continue;}
            const auto over=gpu.execute(c.slot,2).rgba;
            for(unsigned i=0;i<4;++i)nearSample(floating(over[i]),c.expected[i]+c.background[i]*(1-c.expected[3]),"real contribution source-over",stepTolerance);
            if(c.expected[3]==0&&std::abs(c.expected[0])+std::abs(c.expected[1])+std::abs(c.expected[2])>.01f){
                require(output[0]!=bits(0.f)||output[1]!=bits(0.f)||output[2]!=bits(0.f),"A0 additive contribution must survive");++additive;
            }
            // Independent original PS samples at retargeted coordinates. Its
            // alpha is always opaque; divide out ONLY its coordinate aperture.
            gpu.set=gpu.sourceSets[c.slot];
            if(r.colorAperture>.001f){Case p=c.native;p.u=r.colorU;
                const auto oracle=gpu.drawSingle(p,gpu.original);
                for(unsigned i=0;i<3;++i)near(floating(output[i]),floating(oracle[i])/r.colorAperture*r.originalAperture,"independent original native color oracle");
                ++colorOracles;
            }
            if(r.maskAperture>.001f){Case p=c.native;p.u=r.maskU;p.tint={1,1,1,1};const auto oracle=gpu.drawSingle(p,gpu.original);
                float raw=0;for(unsigned i=0;i<3;++i)raw+=floating(oracle[i])*r.inverseG[i];raw/=r.maskAperture;
                near(floating(output[3]),std::max(0.f,std::min(1.f,raw))*r.originalAperture,"independent original raw-G oracle");++maskOracles;
            }
            unequal+=std::abs(r.colorWeight-r.maskWeight)>.01f;seams+=c.mode==9;
            selectors|=1u<<c.native.eye;branches|=1u<<((c.native.flags>>17)&3);++valid;
        }
        require(!enabled||(coordinates?(valid&&invalid&&seams):(valid&&invalid&&additive&&seams&&unequal&&selectors==3&&branches==15&&colorOracles>=8&&maskOracles>=8)),"complete scene positive and original-oracle coverage");
        require(validationErrors.load()==0,"Vulkan/sync validation clean");
        std::cout<<"{\"cases\":"<<cases.size()<<",\"valid\":"<<valid<<",\"invalid_exact_original\":"<<invalid
            <<",\"A0_contribution\":"<<additive<<",\"seam_edge_cases\":"<<seams<<",\"unequal_weights\":"<<unequal
            <<",\"original_color_oracles\":"<<colorOracles<<",\"original_mask_oracles\":"<<maskOracles
            <<",\"max_error\":"<<maxError<<",\"validation_errors\":"<<validationErrors.load()<<",\"checks\":"<<checks
            <<",\"sub_texel_precision_bits\":"<<gpu.properties.limits.subTexelPrecisionBits<<",\"native_source_or_frame_proven\":false,\"output_rgba\":[";
        for(unsigned i=0;i<observedImage.size();++i){if(i)std::cout<<',';const auto& row=observedImage[i];const auto& p=row.second;std::cout<<'['<<row.first<<','<<p[0]<<','<<p[1]<<','<<p[2]<<','<<p[3]<<']';}
        std::cout<<"]}\n";return 0;
    }catch(const std::exception& e){std::cerr<<"scene fixture failed: "<<e.what()<<"; checks="<<checks<<'\n';return 1;}
}
