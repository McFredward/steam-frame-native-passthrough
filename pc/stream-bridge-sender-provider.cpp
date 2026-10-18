// Version-pinned Windows provider. Explicit settings opt in to authenticated
// discovery/control; image coverage still requires its separate exact GPU route.
// Only this exact vrlink module's future public NVENC factory is intercepted.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <openvr_driver.h>
#include "stream-bridge-sender-provider-core.h"
#include "stream-bridge-sender-provider-directmode.h"
#include "stream-bridge-sender-provider-runtime.h"
#include "stream-bridge-sender-provider-coverage.h"
#include "stream-bridge-sender-provider-sourceipc.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using stream_bridge::sender::NvencBoundary;
using stream_bridge::sender::NvencFactory;
constexpr std::array<std::uint8_t,32> ExpectedHash{{0x1e,0x84,0x6b,0xe0,0x41,0x03,0x97,0x98,
 0x58,0x0a,0x91,0x45,0x23,0x71,0xf6,0x28,0x8c,0x61,0x4f,0xb4,0xc1,0x52,0x12,0x6b,
 0x13,0x7e,0x2c,0x69,0x3f,0xb7,0x7e,0xef}};
constexpr DWORD IatRva=0xc2c388,ImageMinimum=0xc3daf4;
using ProcAddress = FARPROC (WINAPI*)(HMODULE,LPCSTR);
ProcAddress const OriginalGetProcAddress=&::GetProcAddress;
std::atomic<NvencFactory> originalFactory{nullptr};
std::atomic<unsigned> bootstrapStatus{0};
NvencBoundary& boundary(){static auto* value=new NvencBoundary(ExpectedHash);return *value;}
stream_bridge::sender::DirectModeBoundary& directMode(){static auto* value=new stream_bridge::sender::DirectModeBoundary;return *value;}
HMODULE nvencLifetime=nullptr; // Never released while public callbacks can exist.
std::mutex factoryMutex;
template<class To,class From> To pointerBits(From from){static_assert(sizeof(To)==sizeof(From),"Win64 pointers");To to{};std::memcpy(&to,&from,sizeof(to));return to;}
NVENCSTATUS NVENCAPI factory(NV_ENCODE_API_FUNCTION_LIST* table){
    return boundary().create(originalFactory.load(std::memory_order_acquire),table);
}
FARPROC WINAPI procAddress(HMODULE module,LPCSTR name){
    // Forward first and preserve original GetLastError, including ordinal calls.
    const auto result=OriginalGetProcAddress(module,name);const auto error=GetLastError();
    if(result&&reinterpret_cast<std::uintptr_t>(name)>0xffff&&
       std::strcmp(name,"NvEncodeAPICreateInstance")==0&&
       module==GetModuleHandleW(L"nvEncodeAPI64.dll")){
        std::lock_guard<std::mutex> guard(factoryMutex);
        const auto candidate=pointerBits<NvencFactory>(result);auto current=originalFactory.load(std::memory_order_acquire);
        if(!current){
            HMODULE retained=nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                pointerBits<LPCWSTR>(result),&retained)){
                nvencLifetime=retained;originalFactory.store(candidate,std::memory_order_release);current=candidate;
            }
        }
        if(current==candidate){bootstrapStatus.store(5);SetLastError(error);return pointerBits<FARPROC>(&factory);}
    }
    SetLastError(error);return result;
}
bool hashFile(HANDLE file,std::array<std::uint8_t,32>& hash){
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE handle=nullptr;
    DWORD objectBytes=0,count=0;bool okay=false;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&count,0)>=0&&objectBytes<=65536){
        std::vector<std::uint8_t> object(objectBytes),buffer(65536);
        if(BCryptCreateHash(alg,&handle,object.data(),objectBytes,nullptr,0,0)>=0){
            okay=true;DWORD bytes=0;
            for(;;){
                if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)){okay=false;break;}
                if(!bytes)break;
                if(BCryptHashData(handle,buffer.data(),bytes,0)<0){okay=false;break;}
            }
            if(okay)okay=BCryptFinishHash(handle,hash.data(),static_cast<ULONG>(hash.size()),0)>=0;
            BCryptDestroyHash(handle);
        }
    }
    BCryptCloseAlgorithmProvider(alg,0);return okay;
}
bool hashMemory(const std::uint8_t* data,ULONG bytes,std::array<std::uint8_t,32>& hash){
    BCRYPT_ALG_HANDLE alg=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    const bool result=BCryptHash(alg,nullptr,0,const_cast<PUCHAR>(data),bytes,hash.data(),static_cast<ULONG>(hash.size()))>=0;
    BCryptCloseAlgorithmProvider(alg,0);return result;
}
bool imageProof(HMODULE module,void*** slot){
    auto* base=reinterpret_cast<const std::uint8_t*>(module);
    // Read fixed public PE fields only after a bounded VirtualQuery region check.
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(base,&memory,sizeof(memory))||memory.AllocationBase!=module||memory.RegionSize<4096)return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>2048)return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
       nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||
       nt->OptionalHeader.SizeOfImage<ImageMinimum||nt->OptionalHeader.SizeOfImage>64*1024*1024)return false;
    // The exact executable section has no base-relocation entries in this pin.
    // Check its loaded bytes too: a file-name/hash match alone is not a live-code proof.
    constexpr std::array<std::uint8_t,32> textHash{{0x6e,0x3c,0x8b,0xfc,0x59,0x94,0x41,0x56,0x1e,0x4c,0xb4,0x0e,0x54,0x2b,0xc9,0x2d,0x93,0x07,0xd9,0xde,0x67,0xdf,0x0f,0x94,0xf2,0xb8,0x7d,0x77,0xfe,0x92,0x4c,0xa2}};
    std::array<std::uint8_t,32> observed{};
    if(!hashMemory(base+0x1000,12754860,observed)||observed!=textHash)return false;
    // The executable hash also pins the import-tail helper; no private
    // instruction body is embedded or called as a private ABI here.
    if(std::memcmp(base+0xc3dad8,"NvEncodeAPICreateInstance",25)!=0)return false;
    auto* address=const_cast<std::uint8_t*>(base)+IatRva;
    if(!VirtualQuery(address,&memory,sizeof(memory))||memory.AllocationBase!=module||
       memory.State!=MEM_COMMIT||memory.Protect&PAGE_GUARD||memory.Protect==PAGE_NOACCESS)return false;
    *slot=reinterpret_cast<void**>(address);
    return **slot==pointerBits<void*>(OriginalGetProcAddress);
}
class Provider final:public vr::IServerTrackedDeviceProvider {
    std::mutex mutex_;std::condition_variable changed_;std::thread worker_;
    bool stopped_=false,initialized_=false;void** slot_=nullptr;HMODULE source_=nullptr;HANDLE file_=INVALID_HANDLE_VALUE;
    vr::IVRDriverContext* context_=nullptr;vr::IVRDriverLog* log_=nullptr;
    stream_bridge::sender::SenderRuntime runtime_{boundary()};stream_bridge::ControlRequest request_{};
    stream_bridge::sender::SenderCoverage coverage_{directMode(),boundary(),runtime_.control()};
    stream_bridge::sender::SourceIpcServer sourceIpc_{[](std::uint32_t pid,void* context){return static_cast<Provider*>(context)->coverage_.ready(pid);},this};
    void** directSlots_=nullptr;std::array<void*,8> directOriginal_{},directWrapped_{};
    void report(const char* status){if(log_){char text[384]{};std::snprintf(text,sizeof text,"Steam Link Native Passthrough: %s",status);log_->Log(text);}}
    bool configure(){
        vr::EVRInitError error=vr::VRInitError_None;
        auto* settings=static_cast<vr::IVRSettings*>(context_->GetGenericInterface(vr::IVRSettings_Version,&error));
        log_=static_cast<vr::IVRDriverLog*>(context_->GetGenericInterface(vr::IVRDriverLog_Version,&error));
        if(!settings)return false;
        vr::EVRSettingsError e=vr::VRSettingsError_None;constexpr auto section="driver_steam_link_native_passthrough";
        if(!settings->GetBool(section,"enableStreamBridge",&e)||e!=vr::VRSettingsError_None){report("bridge disabled; ordinary video");return true;}
        request_.role=stream_bridge::Role::Producer;request_.buildId=712;request_.offered=request_.required=stream_bridge::KnownCapabilities;
        settings->GetString(section,"controlListenAddress",request_.address.data(),static_cast<std::uint32_t>(request_.address.size()),&e);
        if(e!=vr::VRSettingsError_None){report("explicit listen address missing; ordinary video");return false;}
        const auto port=settings->GetInt32(section,"controlPort",&e);
        if(e!=vr::VRSettingsError_None||port<1||port>65535){report("explicit control port invalid; ordinary video");return false;}
        PWSTR appData=nullptr;const HRESULT found=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&appData);
        if(FAILED(found)||!appData){report("private user data unavailable; ordinary video");return false;}
        try {request_.pairingsDirectory=std::filesystem::path(appData)/L"steam-link-native-passthrough"/L"pairings";}
        catch(...){CoTaskMemFree(appData);return false;}
        CoTaskMemFree(appData);request_.port=static_cast<std::uint16_t>(port);request_.explicitlyEnabled=true;runtime_.configure(request_);return true;
    }
    void restoreDirect(){
        directMode().stop();if(!directSlots_)return;
        DWORD old=0;if(VirtualProtect(directSlots_,sizeof(void*)*8,PAGE_READWRITE,&old)){
            for(unsigned i=0;i<8;++i)InterlockedCompareExchangePointer(directSlots_+i,directOriginal_[i],directWrapped_[i]);
            DWORD ignored=0;VirtualProtect(directSlots_,sizeof(void*)*8,old,&ignored);
        }directSlots_=nullptr;
    }
    bool attachDirect(HMODULE module){
        constexpr std::array<std::uint32_t,8> rvas{{0x9f40f0,0x9f4920,0x9f45a0,0x9e5da0,0x9ed780,0x9ea110,0x9e95c0,0x9e5850}};
        auto* base=reinterpret_cast<std::uint8_t*>(module);auto* slots=reinterpret_cast<void**>(base+0xc349b8);
        for(unsigned i=0;i<8;++i){directOriginal_[i]=base+rvas[i];if(slots[i]!=directOriginal_[i])return false;}
        stream_bridge::sender::DirectModeFunctions original{};static_assert(sizeof(original)==sizeof(directOriginal_),"public DirectMode009 table layout");
        original.create=pointerBits<decltype(original.create)>(directOriginal_[0]);original.destroy=pointerBits<decltype(original.destroy)>(directOriginal_[1]);
        original.destroyAll=pointerBits<decltype(original.destroyAll)>(directOriginal_[2]);original.next=pointerBits<decltype(original.next)>(directOriginal_[3]);
        original.submit=pointerBits<decltype(original.submit)>(directOriginal_[4]);original.present=pointerBits<decltype(original.present)>(directOriginal_[5]);
        original.post=pointerBits<decltype(original.post)>(directOriginal_[6]);original.timing=pointerBits<decltype(original.timing)>(directOriginal_[7]);
        if(!directMode().prepare(original))return false;
        const auto wrapped=directMode().entryPoints();std::memcpy(directWrapped_.data(),&wrapped,sizeof wrapped);
        DWORD old=0;if(!VirtualProtect(slots,sizeof(void*)*8,PAGE_READWRITE,&old)){directMode().stop();return false;}
        unsigned installed=0;for(;installed<8;++installed)if(InterlockedCompareExchangePointer(slots+installed,directWrapped_[installed],directOriginal_[installed])!=directOriginal_[installed])break;
        DWORD ignored=0;const bool restored=VirtualProtect(slots,sizeof(void*)*8,old,&ignored)!=0;
        if(installed!=8||!restored){
            DWORD undo=0;if(VirtualProtect(slots,sizeof(void*)*8,PAGE_READWRITE,&undo)){for(unsigned i=0;i<installed;++i)InterlockedCompareExchangePointer(slots+i,directOriginal_[i],directWrapped_[i]);VirtualProtect(slots,sizeof(void*)*8,old,&ignored);}
            directMode().stop();return false;
        }
        directSlots_=slots;return true;
    }
    bool attach(HMODULE candidate){
        wchar_t path[32768]{};const auto n=GetModuleFileNameW(candidate,path,32768);
        if(!n||n>=32768)return false;
        file_=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file_==INVALID_HANDLE_VALUE)return false;
        LARGE_INTEGER size{};std::array<std::uint8_t,32> hash{};
        if(!GetFileSizeEx(file_,&size)||size.QuadPart!=13908632||!hashFile(file_,hash)||hash!=ExpectedHash||!imageProof(candidate,&slot_))return false;
        HMODULE self=nullptr;
        // Public function-list pointers may outlive IAT restoration and Cleanup.
        // Pin our DLL for the process lifetime BEFORE publishing any callback.
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            pointerBits<LPCWSTR>(&procAddress),&self))return false;
        (void)boundary();
        if(request_.explicitlyEnabled&&(!attachDirect(candidate)||!coverage_.configure(reinterpret_cast<std::uintptr_t>(candidate))))return false;
        DWORD old=0;if(!VirtualProtect(slot_,sizeof(void*),PAGE_READWRITE,&old)){restoreDirect();return false;}
        const auto previous=InterlockedCompareExchangePointer(slot_,pointerBits<void*>(&procAddress),pointerBits<void*>(OriginalGetProcAddress));
        DWORD ignored=0;const bool protection=VirtualProtect(slot_,sizeof(void*),old,&ignored)!=0;
        if(previous!=pointerBits<void*>(OriginalGetProcAddress)){slot_=nullptr;restoreDirect();return false;}
        if(!protection){ // Undo our entry; never leave a successful partial activation.
            InterlockedCompareExchangePointer(slot_,pointerBits<void*>(OriginalGetProcAddress),pointerBits<void*>(&procAddress));
            slot_=nullptr;restoreDirect();return false;
        }
        bootstrapStatus.store(4);return true; // Armed; future factory call not yet observed.
    }
    void run(){
        bootstrapStatus.store(1);
        if(!configure()){bootstrapStatus.store(3);return;}
        bool attached=false;
        for(;;){
            {std::lock_guard<std::mutex> guard(mutex_);if(stopped_)return;}
            HMODULE candidate=nullptr;
            if(GetModuleHandleExW(0,L"driver_vrlink.dll",&candidate)){
                source_=candidate;
                try {attached=attach(candidate);if(!attached){bootstrapStatus.store(3);report("native sender attachment refused; ordinary video");}}
                catch(...){bootstrapStatus.store(3);report("native sender attachment failed; ordinary video");}
                break; // One actual module attempt per provider epoch, no automatic retry.
            }
            std::unique_lock<std::mutex> guard(mutex_);changed_.wait_for(guard,std::chrono::milliseconds(250),[this]{return stopped_;});
        }
        if(!attached)return;
        report("pinned native sender attached");
        auto last=stream_bridge::sender::RuntimeState::Off;const char* lastCoverage=nullptr;
        std::array<const char*,32> reported{};
        // Repeated startup churn must not spend the budget before a later
        // handshake/refusal becomes visible. Each distinct state is logged once.
        const auto reportState=[&](const char* value){for(auto& previous:reported){
            if(previous&&std::strcmp(previous,value)==0)return;
            if(!previous){previous=value;report(value);return;}
        }};
        std::uint64_t nextReconfigureReport=1;unsigned reconfigureReports=0;
        bool reportedSuccess=false,reportedRejection=false,reportedRevoke=false;
        const auto reportReconfigure=[&](bool final){
            const auto stats=boundary().reconfigureStats();if(!stats.total)return;
            const bool newOutcome=(!reportedSuccess&&stats.preservedSuccessful)||(!reportedRejection&&stats.preservedRejected)||(!reportedRevoke&&stats.revoked);
            if(!final&&((!newOutcome&&stats.total<nextReconfigureReport)||reconfigureReports>=8))return;
            char text[288]{};std::snprintf(text,sizeof text,
                "encoder reconfigure: calls=%llu; same-contract accepted=%llu, rejected=%llu; revoked=%llu; last native status=%d, extent=%ux%u, contract-known=%u, same=%u, reset=%u",
                static_cast<unsigned long long>(stats.total),static_cast<unsigned long long>(stats.preservedSuccessful),
                static_cast<unsigned long long>(stats.preservedRejected),static_cast<unsigned long long>(stats.revoked),stats.lastStatus,stats.lastWidth,stats.lastHeight,
                unsigned(stats.lastContractKnown),unsigned(stats.lastSameContract),unsigned(stats.lastResetEncoder));
            report(text);++reconfigureReports;
            reportedSuccess=stats.preservedSuccessful!=0;reportedRejection=stats.preservedRejected!=0;reportedRevoke=stats.revoked!=0;
            nextReconfigureReport=stats.total>~std::uint64_t(0)/8?~std::uint64_t(0):stats.total*8;
        };
        // Atomic metadata is sampled/formatted only here, never inside a D3D
        // callback. Refusal populations distinguish the actual first gate from
        // secondary frame-epoch/copy failures without guessing source support.
        std::uint64_t seenSignals=0,seenRefusals=0,nextGateSample=1;
        unsigned gateTransitions=0,gateSamples=0;
        auto nextGateReport=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        const auto reportCoverage=[&](bool final){
            const auto now=std::chrono::steady_clock::now();if(!final&&now<nextGateReport)return;
            const auto stats=coverage_.gateStats();
            const std::uint64_t signals[]{stats.deviceOpened,stats.contextsReady,stats.hookDraws,stats.nativeDraws,
                stats.sourceFrames,stats.constantMaps,stats.constantCaptures,stats.eyeDraws,stats.completePairs,
                stats.nativeCopies,stats.capturedCopies,stats.matchedInputs,stats.certifiedMasks,stats.nativeMaps,stats.inventoryRefreshes};
            std::uint64_t signalBits=0,refusalBits=0;
            for(unsigned i=0;i<sizeof signals/sizeof signals[0];++i)if(signals[i])signalBits|=std::uint64_t(1)<<i;
            std::array<unsigned,3> top{};
            for(unsigned i=1;i<stats.refusals.size();++i){
                if(stats.refusals[i])refusalBits|=std::uint64_t(1)<<i;
                unsigned candidate=i;for(auto& at:top)if(stats.refusals[candidate]>stats.refusals[at])std::swap(candidate,at);
            }
            const bool changed=(signalBits&~seenSignals)||(refusalBits&~seenRefusals);
            const bool sample=stats.hookDraws>=nextGateSample;
            if(!signalBits&&!refusalBits)return;
            if(!final&&(!changed||gateTransitions>=16)&&(!sample||gateSamples>=8))return;
            if(changed)++gateTransitions;
            if(sample)++gateSamples;
            seenSignals|=signalBits;seenRefusals|=refusalBits;
            nextGateSample=stats.hookDraws>~std::uint64_t(0)/8?~std::uint64_t(0):stats.hookDraws*8;
            nextGateReport=now+std::chrono::seconds(5);
            char text[344]{};std::snprintf(text,sizeof text,
                "coverage gates: device=%llu, context=%llu, draws=%llu/%llu, source=%llu, constants=%llu/%llu, eyes=%llu, pairs=%llu, copies=%llu/%llu, inputs=%llu, masks=%llu; last=%s",
                static_cast<unsigned long long>(stats.deviceOpened),static_cast<unsigned long long>(stats.contextsReady),
                static_cast<unsigned long long>(stats.nativeDraws),static_cast<unsigned long long>(stats.hookDraws),
                static_cast<unsigned long long>(stats.sourceFrames),static_cast<unsigned long long>(stats.constantCaptures),static_cast<unsigned long long>(stats.constantMaps),
                static_cast<unsigned long long>(stats.eyeDraws),static_cast<unsigned long long>(stats.completePairs),
                static_cast<unsigned long long>(stats.capturedCopies),static_cast<unsigned long long>(stats.nativeCopies),
                static_cast<unsigned long long>(stats.matchedInputs),static_cast<unsigned long long>(stats.certifiedMasks),
                stream_bridge::sender::coverageGateName(stats.lastRefusal));report(text);
            std::snprintf(text,sizeof text,
                "coverage gate detail: %s=%llu, %s=%llu, %s=%llu; constant-bytes=%u; source=%ux%u/f%u, output=%ux%u/f%u; maps=%llu, inventory=%llu",
                stream_bridge::sender::coverageGateName(static_cast<stream_bridge::sender::CoverageGate>(top[0])),static_cast<unsigned long long>(stats.refusals[top[0]]),
                stream_bridge::sender::coverageGateName(static_cast<stream_bridge::sender::CoverageGate>(top[1])),static_cast<unsigned long long>(stats.refusals[top[1]]),
                stream_bridge::sender::coverageGateName(static_cast<stream_bridge::sender::CoverageGate>(top[2])),static_cast<unsigned long long>(stats.refusals[top[2]]),
                stats.lastConstantBytes,stats.lastSourceWidth,stats.lastSourceHeight,stats.lastSourceFormat,stats.lastOutputWidth,stats.lastOutputHeight,stats.lastOutputFormat,
                static_cast<unsigned long long>(stats.nativeMaps),static_cast<unsigned long long>(stats.inventoryRefreshes));report(text);
            // Public COM table metadata only; never query a new immediate
            // context here, since even a diagnostic getter may change dispatch.
            std::snprintf(text,sizeof text,
                "coverage context: hook-slots=0x%x, table-readable=%u, table-stable=%u, native-device-pointer=%u",
                stats.contextHookSlots,unsigned(stats.contextTableReadable),unsigned(stats.contextTableStable),unsigned(stats.nativeDeviceSame));report(text);
        };
        std::uint32_t reportedFailures=0;
        const auto reportFailure=[&]{
            const auto failure=runtime_.lastFailure();
            if(failure.reason==stream_bridge::sender::RuntimeFailure::None)return;
            const auto bit=std::uint32_t(1)<<static_cast<unsigned>(failure.reason);
            if(reportedFailures&bit)return;
            reportedFailures|=bit;
            char text[312]{};std::snprintf(text,sizeof text,
                "sender refusal: reason=%s; control=%s; protocol-checked=%u, status=%u; encoder-generation=%llu, extent=%ux%u, encoded=%u, confirmed=%u, probe=%u; output-leases=%u",
                stream_bridge::sender::runtimeFailureName(failure.reason),stream_bridge::controlStatusName(failure.control),
                unsigned(failure.hasProtocolStatus),static_cast<unsigned>(failure.protocol),
                static_cast<unsigned long long>(failure.encoder.generation),failure.encoder.width,failure.encoder.height,
                unsigned(failure.encoder.encoded),unsigned(failure.encoder.peerConfirmed),unsigned(failure.encoder.probeObserved),failure.liveOutputLeases);
            report(text);
        };
        for(;;){
            {std::lock_guard<std::mutex> guard(mutex_);if(stopped_)break;}
            try{coverage_.tick();runtime_.tick();
                if(request_.explicitlyEnabled){stream_bridge::LocalConfig config;stream_bridge::LocalSendAuthority authority;stream_bridge::sender::SourceIpcState state;
                    const auto encoder=runtime_.encoder();
                    if(encoder.peerConfirmed&&encoder.probeObserved&&runtime_.control().peerVerified()&&runtime_.control().localConfig(config,authority)){state.binding=authority.channel.binding;state.generation=encoder.generation;state.compatible=true;}
                    sourceIpc_.tick(state);
                }
            }catch(...){sourceIpc_.stop();coverage_.stop();runtime_.stop();report("worker refused; ordinary video");break;}
            const auto* coverageState=coverage_.stateName();if(request_.explicitlyEnabled&&coverageState!=lastCoverage){lastCoverage=coverageState;reportState(coverageState);}
            if(runtime_.state()!=last){last=runtime_.state();reportState(stream_bridge::sender::runtimeStateName(last));}
            if(request_.explicitlyEnabled){reportReconfigure(false);reportCoverage(false);reportFailure();}
            std::unique_lock<std::mutex> guard(mutex_);changed_.wait_for(guard,std::chrono::milliseconds(8),[this]{return stopped_;});
        }
        report("sender worker stopping");
        report(stream_bridge::sender::runtimeStateName(runtime_.state()));
        report(coverage_.stateName());
        reportReconfigure(true);
        reportCoverage(true);
        reportFailure();
        sourceIpc_.stop();runtime_.stop();
    }
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context)override{
        if(!context||initialized_)return vr::VRInitError_Driver_Failed;
        // This observer provides no HMD/device and makes no OpenVR property calls.
        context_=context;initialized_=true;try{worker_=std::thread([this]{run();});}
        catch(...){bootstrapStatus.store(3);return vr::VRInitError_Driver_Failed;}
        return vr::VRInitError_None;
    }
    void Cleanup()override{
        {std::lock_guard<std::mutex> guard(mutex_);stopped_=true;}changed_.notify_all();
        if(worker_.joinable())worker_.join();
        sourceIpc_.stop();coverage_.stop();runtime_.stop();boundary().stop();restoreDirect();
        if(slot_){DWORD old=0;if(VirtualProtect(slot_,sizeof(void*),PAGE_READWRITE,&old)){
            // Another hook's later change is not ours to overwrite.
            InterlockedCompareExchangePointer(slot_,pointerBits<void*>(OriginalGetProcAddress),pointerBits<void*>(&procAddress));
            DWORD ignored=0;VirtualProtect(slot_,sizeof(void*),old,&ignored);}slot_=nullptr;}
        if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}
        // Retain source_ and nvencLifetime through process termination: callbacks
        // and cached native function tables can still be invoked after Cleanup.
        bootstrapStatus.store(6);
    }
    const char*const* GetInterfaceVersions()override{static const char*const versions[]{vr::IServerTrackedDeviceProvider_Version,nullptr};return versions;}
    void RunFrame()override{}
    bool ShouldBlockStandbyMode()override{return false;}
    void EnterStandby()override{}
    void LeaveStandby()override{}
};
Provider& getProvider(){static auto* value=new Provider;return *value;}
class CompositorProvider final:public vr::IVRCompositorPluginProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context)override{return getProvider().Init(context);}
    void Cleanup()override{getProvider().Cleanup();}
    const char*const* GetInterfaceVersions()override{static const char*const versions[]{vr::IVRCompositorPluginProvider_Version,nullptr};return versions;}
    void* GetComponent(const char*)override{return nullptr;} // No HMD/device/renderer replacement.
};
CompositorProvider& getCompositorProvider(){static auto* value=new CompositorProvider;return *value;}
}
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name,int* error){
    if(name&&std::strcmp(name,vr::IServerTrackedDeviceProvider_Version)==0){if(error)*error=vr::VRInitError_None;return &getProvider();}
    if(name&&std::strcmp(name,vr::IVRCompositorPluginProvider_Version)==0){if(error)*error=vr::VRInitError_None;return &getCompositorProvider();}
    if(error)*error=vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
// Read-only bounded metadata; no control/authority API is exported.
extern "C" __declspec(dllexport) unsigned SFNP_SenderBootstrapStatusV1(){return bootstrapStatus.load();}
