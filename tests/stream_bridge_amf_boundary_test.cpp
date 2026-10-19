#include "pc/stream-bridge-amf-boundary.h"
#include <components/VideoEncoderHEVC.h>
#include <algorithm>
#include <cerrno>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
namespace sender=stream_bridge::sender;
namespace {
unsigned checks=0;
void check(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
template<class Interface> struct Properties:Interface {
    amf_long references=1;bool propertyFailure=false;
    std::map<std::wstring,amf::AMFVariant> properties;
    amf_long AMF_STD_CALL Acquire()override{return ++references;}
    amf_long AMF_STD_CALL Release()override{check(references>0,"AMF reference never released twice");return --references;}
    AMF_RESULT AMF_STD_CALL QueryInterface(const amf::AMFGuid&,void** out)override{if(out)*out=nullptr;return AMF_NO_INTERFACE;}
    AMF_RESULT AMF_STD_CALL SetProperty(const wchar_t* key,amf::AMFVariantStruct value)override{
        if(propertyFailure)return AMF_FAIL;
        properties[key]=amf::AMFVariant(value);return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL GetProperty(const wchar_t* key,amf::AMFVariantStruct* out)const override{
        auto i=properties.find(key);if(i==properties.end())return AMF_NOT_FOUND;
        return amf::AMFVariantCopy(out,&i->second);
    }
    amf_bool AMF_STD_CALL HasProperty(const wchar_t* key)const override{return properties.count(key)!=0;}
    amf_size AMF_STD_CALL GetPropertyCount()const override{return properties.size();}
    AMF_RESULT AMF_STD_CALL GetPropertyAt(amf_size,wchar_t*,amf_size,amf::AMFVariantStruct*)const override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL Clear()override{properties.clear();return AMF_OK;}
    AMF_RESULT AMF_STD_CALL AddTo(amf::AMFPropertyStorage*,amf_bool,amf_bool)const override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL CopyTo(amf::AMFPropertyStorage*,amf_bool)const override{return AMF_NOT_SUPPORTED;}
    void AMF_STD_CALL AddObserver(amf::AMFPropertyStorageObserver*)override{}
    void AMF_STD_CALL RemoveObserver(amf::AMFPropertyStorageObserver*)override{}
};
template<class Interface> struct Data:Properties<Interface> {
    amf::AMF_DATA_TYPE type=amf::AMF_DATA_SURFACE;amf_pts pts=0,duration=0;
    amf::AMF_MEMORY_TYPE AMF_STD_CALL GetMemoryType()override{return amf::AMF_MEMORY_HOST;}
    AMF_RESULT AMF_STD_CALL Duplicate(amf::AMF_MEMORY_TYPE,amf::AMFData**)override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL Convert(amf::AMF_MEMORY_TYPE)override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL Interop(amf::AMF_MEMORY_TYPE)override{return AMF_NOT_SUPPORTED;}
    amf::AMF_DATA_TYPE AMF_STD_CALL GetDataType()override{return type;}
    amf_bool AMF_STD_CALL IsReusable()override{return false;}
    void AMF_STD_CALL SetPts(amf_pts value)override{pts=value;}
    amf_pts AMF_STD_CALL GetPts()override{return pts;}
    void AMF_STD_CALL SetDuration(amf_pts value)override{duration=value;}
    amf_pts AMF_STD_CALL GetDuration()override{return duration;}
};
using Surface=Data<amf::AMFData>;
struct Buffer:Data<amf::AMFBuffer> {
    std::array<std::uint8_t,9> bytes{{0,0,0,1,0x26,1,0x80,0x11,0x22}};
    Buffer(){type=amf::AMF_DATA_BUFFER;references=0;}
    AMF_RESULT AMF_STD_CALL QueryInterface(const amf::AMFGuid& iid,void** out)override{
        if(!out)return AMF_INVALID_ARG;
        if(iid==amf::AMFBuffer::IID()){*out=static_cast<amf::AMFBuffer*>(this);Acquire();return AMF_OK;}
        *out=nullptr;return AMF_NO_INTERFACE;
    }
    AMF_RESULT AMF_STD_CALL SetSize(amf_size)override{return AMF_NOT_SUPPORTED;}
    amf_size AMF_STD_CALL GetSize()override{return bytes.size();}
    void* AMF_STD_CALL GetNative()override{return bytes.data();}
    void AMF_STD_CALL AddObserver(amf::AMFBufferObserver*)override{}
    void AMF_STD_CALL RemoveObserver(amf::AMFBufferObserver*)override{}
};
struct Component:Properties<amf::AMFComponent> {
    unsigned submitCalls=0,queryCalls=0,flushCalls=0,reinitCalls=0,terminateCalls=0;
    AMF_RESULT submitResult=AMF_OK,emptyResult=AMF_REPEAT,flushResult=AMF_OK,reinitResult=AMF_OK,terminateResult=AMF_OK;
    amf::AMFData* lastInput=nullptr;amf_int32 reinitWidth=0,reinitHeight=0;
    std::deque<Buffer*> queue;std::vector<std::unique_ptr<Buffer>> owned;
    std::function<void()> beforeSubmitReturns;
    Component(){properties[AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE]=amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE_FRAME));}
    Buffer* encoded(const Surface& input){auto buffer=std::make_unique<Buffer>();buffer->properties=input.properties;buffer->pts=input.pts;
        auto* p=buffer.get();owned.push_back(std::move(buffer));return p;}
    amf_size AMF_STD_CALL GetPropertiesInfoCount()const override{return 0;}
    AMF_RESULT AMF_STD_CALL GetPropertyInfo(amf_size,const amf::AMFPropertyInfo**)const override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL GetPropertyInfo(const wchar_t*,const amf::AMFPropertyInfo**)const override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL ValidateProperty(const wchar_t*,amf::AMFVariantStruct,amf::AMFVariantStruct*)const override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL Init(amf::AMF_SURFACE_FORMAT,amf_int32,amf_int32)override{return AMF_OK;}
    AMF_RESULT AMF_STD_CALL ReInit(amf_int32 width,amf_int32 height)override{++reinitCalls;reinitWidth=width;reinitHeight=height;errno=ENOSPC;return reinitResult;}
    AMF_RESULT AMF_STD_CALL Terminate()override{++terminateCalls;errno=ENOSYS;return terminateResult;}
    AMF_RESULT AMF_STD_CALL Drain()override{return AMF_OK;}
    AMF_RESULT AMF_STD_CALL Flush()override{++flushCalls;errno=ENFILE;return flushResult;}
    AMF_RESULT AMF_STD_CALL SubmitInput(amf::AMFData* input)override{++submitCalls;lastInput=input;if(beforeSubmitReturns)beforeSubmitReturns();errno=EAGAIN;return submitResult;}
    AMF_RESULT AMF_STD_CALL QueryOutput(amf::AMFData** output)override{
        ++queryCalls;if(!queue.empty()&&output){*output=queue.front();queue.pop_front();(*output)->Acquire();errno=EIO;return AMF_OK;}
        if(output)*output=nullptr;
        errno=EIO;return emptyResult;
    }
    amf::AMFContext* AMF_STD_CALL GetContext()override{return nullptr;}
    AMF_RESULT AMF_STD_CALL SetOutputDataAllocatorCB(amf::AMFDataAllocatorCB*)override{return AMF_OK;}
    AMF_RESULT AMF_STD_CALL GetCaps(amf::AMFCaps**)override{return AMF_NOT_SUPPORTED;}
    AMF_RESULT AMF_STD_CALL Optimize(amf::AMFComponentOptimizationCallback*)override{return AMF_OK;}
};
struct Fixture {
    Component component;sender::AmfBoundary boundary;std::vector<sender::AmfOutputIdentity> observed;
    static AMF_RESULT AMF_STD_CALL submit(amf::AMFComponent* p,amf::AMFData* d){return p->SubmitInput(d);}
    static AMF_RESULT AMF_STD_CALL query(amf::AMFComponent* p,amf::AMFData** d){return p->QueryOutput(d);}
    static AMF_RESULT AMF_STD_CALL flush(amf::AMFComponent* p){return p->Flush();}
    static AMF_RESULT AMF_STD_CALL reinit(amf::AMFComponent* p,amf_int32 w,amf_int32 h){return p->ReInit(w,h);}
    static AMF_RESULT AMF_STD_CALL terminate(amf::AMFComponent* p){return p->Terminate();}
    static void output(const sender::AmfOutputIdentity& value,void* context)noexcept{
        auto& f=*static_cast<Fixture*>(context);f.observed.push_back(value);
        if(!value.inputObject||!value.output||value.output->GetSize()!=9||!value.output->GetNative())std::terminate();
        errno=EOVERFLOW; // Boundary must restore the native call's errno.
    }
    static sender::AmfContract contract(){return {true,true,true,true,3840,2160,{{123,456}}};}
    static sender::AmfInputIdentity identity(unsigned content){return {0x1234,8,content,3840,2160,true};}
    sender::AmfOriginals originals(){return {submit,query,flush,reinit,terminate};}
    void prepare(){check(boundary.prepare(&component,originals(),contract(),{output,this}),"actual official AMF component contract prepares");}
    void receive(Buffer* buffer){component.queue.push_back(buffer);amf::AMFData* got=nullptr;const auto before=component.queryCalls;
        check(boundary.query(&got)==AMF_OK&&got==buffer&&component.queryCalls==before+1&&errno==EIO,"native QueryOutput result, object, errno and one-call semantics survive observation");
        check(buffer->references==1,"only caller's original returned AMF reference remains after query");got->Release();}
};
void admission(){
    for(unsigned bit=0;bit<8;++bit){Fixture f;auto c=f.contract();
        if(bit==0)c.explicitlyEnabled=false;
        if(bit==1)c.pinnedNativeFactory=false;
        if(bit==2)c.hevcComponent=false;
        if(bit==3)c.completeAnnexBFrames=false;
        if(bit==4)c.width=0;
        if(bit==5)c.height=20000;
        if(bit==6)c.propertyDomain[0]=0;
        if(bit==7)c.propertyDomain[1]=0;
        check(!f.boundary.prepare(&f.component,f.originals(),c)&&f.component.references==1,"unknown/off/malformed source contract never becomes an encoder proof");}
    Fixture sliced;sliced.component.properties[AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE]=amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE_SLICE));
    check(!sliced.boundary.prepare(&sliced.component,sliced.originals(),sliced.contract()),"slice output cannot be certified as complete HEVC access units");
}
void reorderedAndReused(){
    Fixture f;f.prepare();Surface a,b;check(f.component.references==2,"attached AMF component retained");
    a.SetPts(77);b.SetPts(77); // Deliberately identical PTS; never used as identity.
    check(f.boundary.submit(&a,f.identity(10))==AMF_OK&&errno==EAGAIN&&f.component.lastInput==&a,"original SubmitInput accepted without changing caller object or errno");
    check(f.boundary.submit(&b,f.identity(11))==AMF_OK&&f.component.submitCalls==2,"second native input forwarded once");
    auto* aa=f.component.encoded(a);auto* bb=f.component.encoded(b);
    f.receive(bb);f.receive(aa);
    check(f.observed.size()==2&&f.observed[0].input.contentGeneration==11&&f.observed[1].input.contentGeneration==10
        &&f.observed[0].submission!=f.observed[1].submission,"reordered AMF outputs join their own immutable property receipts, never PTS/FIFO/latest guesses");
    check(a.references==1&&b.references==1&&f.boundary.pending()==0,"completed inputs release retained references");
    f.receive(aa);check(f.observed.size()==2,"duplicate native output cannot replay a retired receipt");
    check(f.boundary.submit(&a,f.identity(12))==AMF_OK,"completed native input object can be reused with a fresh cookie");
    auto* next=f.component.encoded(a);f.receive(aa);check(f.observed.size()==2,"late old output never aliases reused AMF input object");f.receive(next);
    check(f.observed.size()==3&&f.observed.back().input.contentGeneration==12,"reused object's current content remains exact");
    f.boundary.stop();check(f.component.references==2,"stop preserves native forwarding callback lifetime");
}
void outputBeforeAcceptance(){
    for(auto result:{AMF_OK,AMF_NEED_MORE_INPUT,AMF_INPUT_FULL,AMF_FAIL}){
        Fixture f;f.prepare();Surface input;f.component.submitResult=result;Buffer* early=nullptr;
        f.component.beforeSubmitReturns=[&]{early=f.component.encoded(input);f.component.queue.push_back(early);amf::AMFData* got=nullptr;
            check(f.boundary.query(&got)==AMF_OK&&got==early,"native output may precede SubmitInput acceptance return");
            check(f.observed.empty()&&early->references==2,"tentative output is held without fabricating successful acceptance");got->Release();};
        check(f.boundary.submit(&input,f.identity(1))==result&&f.component.submitCalls==1,"every original acceptance/error result is unchanged");
        const bool accepted=result==AMF_OK||result==AMF_NEED_MORE_INPUT;
        check(f.observed.size()==(accepted?1u:0u)&&input.references==1&&early->references==0&&f.boundary.pending()==0,
            "only accepted original submissions publish; deferred input/output references always retire");
    }
}
void ambiguityAndFailures(){
    {
    Fixture f;f.prepare();Surface input;
    f.boundary.submit(&input,f.identity(1));auto* old=f.component.encoded(input);
    auto unknown=f.identity(2);unknown.exactSourceAndCoverage=false;f.boundary.submit(&input,unknown);auto* ambiguous=f.component.encoded(input);
    f.receive(ambiguous);f.receive(old);check(f.observed.empty()&&input.references==1&&!f.boundary.active(),"overlapping native AMFData reuse, even by an unproved input, revokes the attachment before late metadata copying can alias new pixels");
    f.boundary.submit(&input,f.identity(3));f.receive(f.component.encoded(input));
    check(f.observed.empty()&&f.boundary.pending()==0,"third reuse cannot rearm a receipt whose earlier native metadata copy is still ambiguous");
    }
    Fixture f;f.prepare();Surface input;
    input.propertyFailure=true;f.boundary.submit(&input,f.identity(3));f.receive(f.component.encoded(input));
    check(f.observed.empty()&&f.component.submitCalls==1,"property failure preserves native encoding but cannot authorize an old cookie");
    input.propertyFailure=false;f.boundary.submit(&input,f.identity(4));auto* forged=f.component.encoded(input);
    forged->properties[L"SteamLinkNativePassthrough.InputV1.Domain1"]=amf::AMFVariant(static_cast<amf_int64>(789));f.receive(forged);
    check(f.observed.empty()&&f.boundary.pending()==1,"wrong source/property domain cannot borrow a pending input");
    f.component.emptyResult=AMF_OK;amf::AMFData* empty=reinterpret_cast<amf::AMFData*>(0x1234);
    check(f.boundary.query(&empty)==AMF_OK&&!empty&&f.observed.empty(),"AMF_OK with no data is not an output receipt");
    f.boundary.stop();check(input.references==1&&!f.boundary.active(),"manual stop releases any unresolved input");
}
void boundedAndLifecycle(){
    Fixture f;f.prepare();std::array<Surface,sender::AmfBoundary::Capacity+1> inputs;
    for(unsigned i=0;i<inputs.size();++i)check(f.boundary.submit(&inputs[i],f.identity(i+1))==AMF_OK,"bounded ledger never prevents original native encoding");
    check(f.boundary.pending()==sender::AmfBoundary::Capacity&&inputs.back().references==1,"pending AMF data lifetime is strictly bounded at32");
    f.receive(f.component.encoded(inputs.back()));check(f.observed.empty(),"capacity refusal cannot guess coverage for untracked input");
    auto* stale=f.component.encoded(inputs[0]);f.component.flushResult=AMF_FAIL;
    check(f.boundary.flush()==AMF_FAIL&&f.boundary.pending()==32&&errno==ENFILE,"failed native Flush does not pretend pending native work was retired");
    f.component.flushResult=AMF_OK;check(f.boundary.flush()==AMF_OK&&f.boundary.pending()==0&&f.boundary.active(),"successful native Flush retires all old generation receipts");
    for(const auto& input:inputs)check(input.references==1,"Flush balances every bounded input reference");
    f.boundary.submit(&inputs[0],f.identity(100));f.receive(stale);check(f.observed.empty(),"late pre-Flush output cannot alias same-object reuse after generation change");
    f.receive(f.component.encoded(inputs[0]));check(f.observed.size()==1&&f.observed[0].encoderGeneration==2,"new post-Flush output carries fresh encoder generation");
    f.component.reinitResult=AMF_FAIL;check(f.boundary.reinit(1920,1080)==AMF_FAIL&&f.boundary.active(),"rejected native ReInit preserves existing proved contract");
    f.component.reinitResult=AMF_OK;check(f.boundary.reinit(1920,1080)==AMF_OK&&!f.boundary.active()&&f.component.reinitWidth==1920&&f.component.reinitHeight==1080&&errno==ENOSPC,"successful ReInit revokes source proof and preserves actual native arguments");
    const auto before=f.component.submitCalls;f.boundary.submit(&inputs[0],f.identity(101));check(f.component.submitCalls==before+1&&f.boundary.pending()==0,"stopped proof still forwards ordinary native encoding");
    check(f.boundary.terminate()==AMF_OK&&f.component.terminateCalls==1&&errno==ENOSYS,"native Terminate forwarded once without inventing output");
}
}
int main(){try{admission();reorderedAndReused();outputBeforeAcceptance();ambiguityAndFailures();boundedAndLifecycle();std::cout<<checks<<" actual public AMF interface controls PASS (no GPU/Steam hook proof)\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
