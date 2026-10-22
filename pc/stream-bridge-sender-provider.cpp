// Public-ABI Windows provider. Explicit settings opt in to authenticated
// discovery/control; coverage still requires exact live GPU/input/eye proofs.
// Native imports and DirectMode RTTI are discovered before publishing hooks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <openvr_driver.h>
#include "stream-bridge-sender-provider-core.h"
#include "stream-bridge-sender-provider-directmode.h"
#include "stream-bridge-sender-provider-runtime.h"
#include "stream-bridge-sender-provider-coverage.h"
#include "stream-bridge-sender-provider-sourceipc.h"
#include "bridge/stream_bridge_log.h"
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
[[maybe_unused]] bool hashFile(HANDLE file,std::array<std::uint8_t,32>& hash){
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
bool valvePublisher(const wchar_t* path,HANDLE handle){
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path;file.hFile=handle;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&file;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;bool okay=false;
    if(WinVerifyTrust(nullptr,&action,&trust)==ERROR_SUCCESS){
        auto* provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        auto* certificate=signer?WTHelperGetProvCertFromChain(signer,0):nullptr;
        wchar_t organization[128]{};
        if(certificate&&certificate->pCert&&CertGetNameStringW(certificate->pCert,CERT_NAME_ATTR_TYPE,0,
            const_cast<char*>(szOID_ORGANIZATION_NAME),organization,128)>1){
            okay=!std::wcscmp(organization,L"Valve Corp.")||!std::wcscmp(organization,L"Valve Corporation");
        }
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&trust);return okay;
}
bool nativeFile(HANDLE file,std::vector<std::uint8_t>& raw){
    LARGE_INTEGER size{},zero{};
    if(!GetFileSizeEx(file,&size)||size.QuadPart<512||size.QuadPart>64*1024*1024||!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return false;
    raw.resize(static_cast<std::size_t>(size.QuadPart));std::size_t total=0;
    while(total<raw.size()){DWORD count=0;
        if(!ReadFile(file,raw.data()+total,static_cast<DWORD>(std::min<std::size_t>(65536,raw.size()-total)),&count,nullptr)||!count)return false;
        total+=count;
    }return true;
}
bool loadedCodeMatches(HMODULE module,const stream_bridge::sender::NativePeImage& image,
    const stream_bridge::sender::NativeImageLayout& layout){
    const auto base=reinterpret_cast<std::uintptr_t>(module);
    for(unsigned i=0;i<layout.executableCount;++i){const auto& range=layout.executable[i];
        const auto* expected=image.at(range.begin,range.size);if(!expected)return false;
        std::vector<std::uint8_t> actual(range.size);SIZE_T count=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(base+range.begin),actual.data(),actual.size(),&count)||count!=actual.size()||
           std::memcmp(actual.data(),expected,actual.size()))return false;
        std::size_t offset=0;while(offset<range.size){MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(reinterpret_cast<const void*>(base+range.begin+offset),&info,sizeof(info))||
               info.AllocationBase!=module||info.State!=MEM_COMMIT||info.Type!=MEM_IMAGE||info.Protect&(PAGE_GUARD|PAGE_NOACCESS)||
               !(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
            const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
            if(end<=base+range.begin+offset)return false;
            offset=std::min<std::size_t>(range.size,end-base-range.begin);
        }
    }return true;
}
class Provider final:public vr::IServerTrackedDeviceProvider {
    std::mutex mutex_;std::condition_variable changed_;std::thread worker_;
    bool stopped_=false,initialized_=false;void** slot_=nullptr;HMODULE source_=nullptr;HANDLE file_=INVALID_HANDLE_VALUE;
    vr::IVRDriverContext* context_=nullptr;vr::IVRDriverLog* log_=nullptr;
    stream_bridge::sender::SenderRuntime runtime_{boundary()};stream_bridge::ControlRequest request_{};
    stream_bridge::sender::SenderCoverage coverage_{directMode(),boundary(),runtime_.control()};
    stream_bridge::sender::SourceIpcServer sourceIpc_{[](std::uint32_t pid,void* context){return static_cast<Provider*>(context)->coverage_.ready(pid);},this,
        [](std::uint32_t pid,bool budget,void* context)noexcept{static_cast<Provider*>(context)->coverage_.sourceProcess(pid,budget);}};
    void** directSlots_=nullptr;std::array<void*,8> directOriginal_{},directWrapped_{};
    void report(const char* status,stream_bridge::logging::Level level=stream_bridge::logging::Level::Important){if(log_&&stream_bridge::logging::enabled(level)){char text[768]{};std::snprintf(text,sizeof text,"Steam Link Native Passthrough: %s",status);log_->Log(text);}}
    bool configure(){
        vr::EVRInitError error=vr::VRInitError_None;
        auto* settings=static_cast<vr::IVRSettings*>(context_->GetGenericInterface(vr::IVRSettings_Version,&error));
        log_=static_cast<vr::IVRDriverLog*>(context_->GetGenericInterface(vr::IVRDriverLog_Version,&error));
        if(!settings)return false;
        vr::EVRSettingsError e=vr::VRSettingsError_None;constexpr auto section="driver_steam_link_native_passthrough";
        if(!settings->GetBool(section,"enableStreamBridge",&e)||e!=vr::VRSettingsError_None){report("bridge disabled; ordinary video");return true;}
        request_.role=stream_bridge::Role::Producer;request_.buildId=712;request_.offered=stream_bridge::KnownCapabilities;request_.required=stream_bridge::NativeRequiredCapabilities;
        settings->GetString(section,"controlListenAddress",request_.address.data(),static_cast<std::uint32_t>(request_.address.size()),&e);
        if(e!=vr::VRSettingsError_None){report("explicit listen address missing; ordinary video",stream_bridge::logging::Level::Warning);return false;}
        const auto port=settings->GetInt32(section,"controlPort",&e);
        if(e!=vr::VRSettingsError_None||port<1||port>65535){report("explicit control port invalid; ordinary video",stream_bridge::logging::Level::Warning);return false;}
        PWSTR appData=nullptr;const HRESULT found=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&appData);
        if(FAILED(found)||!appData){report("private user data unavailable; ordinary video",stream_bridge::logging::Level::Warning);return false;}
        try {const auto root=std::filesystem::path(appData)/L"steam-link-native-passthrough";
            stream_bridge::logging::configureCold(root/L"pc-provider/logging-default.conf",root/L"logging.conf");
            request_.pairingsDirectory=root/L"pairings";}
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
    bool attachDirect(HMODULE module,const stream_bridge::sender::NativeImageLayout* layout=nullptr){
        constexpr std::array<std::uint32_t,8> legacyRvas{{0x9f40f0,0x9f4920,0x9f45a0,0x9e5da0,0x9ed780,0x9ea110,0x9e95c0,0x9e5850}};
        const auto& rvas=layout?layout->directMethods:legacyRvas;
        auto* base=reinterpret_cast<std::uint8_t*>(module);auto* slots=reinterpret_cast<void**>(base+(layout?layout->directTable:0xc349b8));
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
        std::vector<std::uint8_t> raw;std::array<std::uint8_t,32> hash{};
        if(!nativeFile(file_,raw)||!hashMemory(raw.data(),static_cast<ULONG>(raw.size()),hash))return false;
        const bool legacy=hash==ExpectedHash;
        stream_bridge::sender::NativeImageLayout layout;
        if(legacy){if(!imageProof(candidate,&slot_))return false;}
        else {
            if(!valvePublisher(path,file_)){report("capability refused: native module Valve publisher could not be verified",stream_bridge::logging::Level::Warning);return false;}
            stream_bridge::sender::NativePeImage image;const char* why="invalid bounded x64 PE image";
            if(!image.open(raw.data(),raw.size())||!image.discover(layout,why)){report(why);return false;}
            if(!loadedCodeMatches(candidate,image,layout)){report("capability refused: mapped native executable differs from its verified file",stream_bridge::logging::Level::Warning);return false;}
            auto* address=reinterpret_cast<std::uint8_t*>(candidate)+layout.getProcAddress;
            MEMORY_BASIC_INFORMATION memory{};
            if(!VirtualQuery(address,&memory,sizeof(memory))||memory.AllocationBase!=candidate||memory.State!=MEM_COMMIT||
               memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)||memory.RegionSize<sizeof(void*))return false;
            slot_=reinterpret_cast<void**>(address);
            if(*slot_!=pointerBits<void*>(OriginalGetProcAddress))return false;
            if(!boundary().bindVerifiedModule(hash))return false;
            report("native capabilities verified: public NVENC lookup, DirectMode009 RTTI, mapped executable; adaptive native luma/resource association");
        }
        HMODULE self=nullptr;
        // Public function-list pointers may outlive IAT restoration and Cleanup.
        // Pin our DLL for the process lifetime BEFORE publishing any callback.
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            pointerBits<LPCWSTR>(&procAddress),&self))return false;
        (void)boundary();
        if(request_.explicitlyEnabled){
            if(!attachDirect(candidate,legacy?nullptr:&layout))return false;
            if(!(legacy?coverage_.configure(reinterpret_cast<std::uintptr_t>(candidate)):coverage_.configure(reinterpret_cast<std::uintptr_t>(candidate),layout))){
                coverage_.stop();restoreDirect();return false;
            }
        }
        // Retire coverage observation before unpublishing its DirectMode
        // source wrappers. Cached callbacks retain their process-lifetime
        // owners even after this failed attachment has been unwound.
        const auto undoCoverage=[&]{if(request_.explicitlyEnabled)coverage_.stop();restoreDirect();};
        DWORD old=0;if(!VirtualProtect(slot_,sizeof(void*),PAGE_READWRITE,&old)){undoCoverage();return false;}
        const auto previous=InterlockedCompareExchangePointer(slot_,pointerBits<void*>(&procAddress),pointerBits<void*>(OriginalGetProcAddress));
        DWORD ignored=0;const bool protection=VirtualProtect(slot_,sizeof(void*),old,&ignored)!=0;
        if(previous!=pointerBits<void*>(OriginalGetProcAddress)){slot_=nullptr;undoCoverage();return false;}
        if(!protection){ // Undo our entry; never leave a successful partial activation.
            InterlockedCompareExchangePointer(slot_,pointerBits<void*>(OriginalGetProcAddress),pointerBits<void*>(&procAddress));
            slot_=nullptr;undoCoverage();return false;
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
                try {attached=attach(candidate);if(!attached){bootstrapStatus.store(3);report("native sender attachment refused; ordinary video",stream_bridge::logging::Level::Warning);}}
                catch(...){bootstrapStatus.store(3);report("native sender attachment failed; ordinary video",stream_bridge::logging::Level::Error);}
                break; // One actual module attempt per provider epoch, no automatic retry.
            }
            std::unique_lock<std::mutex> guard(mutex_);changed_.wait_for(guard,std::chrono::milliseconds(250),[this]{return stopped_;});
        }
        if(!attached)return;
        report("verified native sender attached");
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
            const auto outputs=boundary().outputStats();
            char text[512]{};std::snprintf(text,sizeof text,
                "encoder reconfigure: calls=%llu; same-contract accepted=%llu, rejected=%llu; revoked=%llu; last native status=%d, extent=%ux%u, contract-known=%u, same=%u, reset=%u; in-call exact input-captures/output-completions=%llu/%llu; output exact/unmatched/early-completed=%llu/%llu/%llu",
                static_cast<unsigned long long>(stats.total),static_cast<unsigned long long>(stats.preservedSuccessful),
                static_cast<unsigned long long>(stats.preservedRejected),static_cast<unsigned long long>(stats.revoked),stats.lastStatus,stats.lastWidth,stats.lastHeight,
                unsigned(stats.lastContractKnown),unsigned(stats.lastSameContract),unsigned(stats.lastResetEncoder),
                static_cast<unsigned long long>(stats.overlappingInputs),static_cast<unsigned long long>(stats.overlappingOutputs),
                static_cast<unsigned long long>(outputs.exact),static_cast<unsigned long long>(outputs.unmatched),
                static_cast<unsigned long long>(outputs.completedBeforeEncodeReturn));
            report(text,stats.preservedRejected?stream_bridge::logging::Level::Warning:stream_bridge::logging::Level::Debug);++reconfigureReports;
            reportedSuccess=stats.preservedSuccessful!=0;reportedRejection=stats.preservedRejected!=0;reportedRevoke=stats.revoked!=0;
            nextReconfigureReport=stats.total>~std::uint64_t(0)/8?~std::uint64_t(0):stats.total*8;
        };
        auto nextBudgetReport=std::chrono::steady_clock::time_point{};std::uint64_t budgetReports=0;
        const auto reportWireBudget=[&](bool final){
            const auto now=std::chrono::steady_clock::now();if(!final&&now<nextBudgetReport)return;
            nextBudgetReport=now+std::chrono::seconds(5);
            const auto stats=boundary().reconfigureStats();if(!stats.budgeted||(!final&&stats.budgeted==budgetReports))return;
            budgetReports=stats.budgeted;char text[512]{};
            std::snprintf(text,sizeof text,"producer wire budget: attempts/accepted/rejected=%llu/%llu/%llu; last video requested/passed=%u/%u bit/s; reserved metadata peak/mean=%u/%u bit/s; VBV requested/passed=%u/%u bits; delay=%u/%u bits; over-budget=%u; producer-v2 only, exact coverage retained",
                static_cast<unsigned long long>(stats.budgeted),static_cast<unsigned long long>(stats.budgetAccepted),static_cast<unsigned long long>(stats.budgetRejected),
                stats.requestedVideoRate,stats.appliedVideoRate,stats.metadataRate,stats.meanMetadataRate,
                stats.requestedVbv,stats.appliedVbv,stats.requestedDelay,stats.appliedDelay,
                unsigned(std::uint64_t(stats.appliedVideoRate)+stats.metadataRate>stats.requestedVideoRate));report(text,(stats.budgetRejected||std::uint64_t(stats.appliedVideoRate)+stats.metadataRate>stats.requestedVideoRate)?stream_bridge::logging::Level::Warning:stream_bridge::logging::Level::Debug);
        };
        // Atomic metadata is sampled/formatted only here, never inside a D3D
        // callback. Refusal populations distinguish the actual first gate from
        // secondary frame-epoch/copy failures without guessing source support.
        std::uint64_t seenSignals=0,seenRefusals=0,nextGateSample=1;
        unsigned gateTransitions=0,gateSamples=0;
        stream_bridge::sender::CoverageSourceReportBudget sourceReports;
        unsigned alphaReports=0,alphaSources=0;
        std::uint32_t alphaPid=0;
        bool alphaUnavailableReported=false;
        auto nextAlphaReport=std::chrono::steady_clock::now();
        auto nextGateReport=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        std::uint64_t lastCleanupRequests=0;
        const auto reportCoverage=[&](bool final){
            const auto now=std::chrono::steady_clock::now();if(!final&&now<nextGateReport)return;
            // Quiet reports must still advance the sampling deadline. Otherwise
            // exhaustion of the verbose report budget polls public COM metadata
            // on every 8 ms worker tick instead of this cold five-second cadence.
            nextGateReport=now+std::chrono::seconds(5);
            const auto stats=coverage_.gateStats();
            // Sample without taking the render/registry mutex. Report cleanup
            // causes before the older finite verbose gate budget can hide them.
            const auto cleanup=directMode().cleanupStats();
            if(final||cleanup.requests!=lastCleanupRequests){
                char cleanupText[256]{};std::snprintf(cleanupText,sizeof cleanupText,
                    "direct cleanup: requests=%llu, empty=%llu, changed=%llu, lock-miss=%llu; requests are not frames",
                    static_cast<unsigned long long>(cleanup.requests),static_cast<unsigned long long>(cleanup.empty),
                    static_cast<unsigned long long>(cleanup.changed),static_cast<unsigned long long>(cleanup.lockMisses));
                report(cleanupText,stream_bridge::logging::Level::Debug);lastCleanupRequests=cleanup.requests;
            }
            // Sample source alpha at the actual native shared-texture input,
            // independently of the native shader's output alpha. A coherent
            // sample contains its own PID/present/eye/extent/CB metadata; it is
            // diagnostic evidence only, never an activation/frame certificate.
            stream_bridge::sender::MaskContentSample mask{};
            static std::uint64_t lastMask=0;static unsigned maskReports=0;
            if(coverage_.maskContent(mask)&&mask.sequence!=lastMask&&(final||maskReports<32)){
                lastMask=mask.sequence;++maskReports;char detail[416]{};
                std::snprintf(detail,sizeof detail,"mask content sample: pid=%u, source-frame=%llu, extent=%ux%u, encoding=%u, bytes=%u, rle-bytes=%u; sampled scene/real/fractional=%u/%u/%u of %u; distinct=%u, horizontal/vertical changes=%u/%u; statistics only, no frame permission",
                    mask.pid,static_cast<unsigned long long>(mask.frame),mask.width,mask.height,mask.encoding,mask.encodedBytes,mask.rleBytes,
                    mask.scene,mask.real,mask.fractional,mask.samples,mask.distinct,mask.horizontal,mask.vertical);
                report(detail,stream_bridge::logging::Level::Debug);
            }
            stream_bridge::sender::SourceAlphaSample alpha[2]{};
            const bool haveAlpha[2]{coverage_.sourceAlpha(0,alpha[0]),coverage_.sourceAlpha(1,alpha[1])};
            const auto observedPid=haveAlpha[0]?alpha[0].source.pid:haveAlpha[1]?alpha[1].source.pid:0;
            const auto alphaError=coverage_.sourceAlphaError();
            if(alphaError&&!observedPid&&!alphaUnavailableReported){
                char alphaText[160]{};std::snprintf(alphaText,sizeof alphaText,
                    "native source alpha: diagnostic unavailable, readback-hr=0x%08x; coverage admission and rendering unchanged",alphaError);
                report(alphaText,stream_bridge::logging::Level::Debug);alphaUnavailableReported=true;
            }
            if(observedPid&&observedPid!=alphaPid&&alphaSources<8){alphaPid=observedPid;++alphaSources;alphaReports=0;nextAlphaReport=now;}
            if(observedPid&&(final||(alphaReports<14&&now>=nextAlphaReport))){
                ++alphaReports;nextAlphaReport=now+std::chrono::seconds(alphaReports<6?5:30);
                for(unsigned eye=0;eye<2;++eye)if(haveAlpha[eye]){
                    const auto& a=alpha[eye];const auto& m=a.source;
                    char alphaText[736]{};int used=std::snprintf(alphaText,sizeof alphaText,
                        "native source alpha: pid=%u, eye=%u, present=%llu, samples=%llu, source=%ux%u/resource-f%u/view-f%u; app-uv=%g,%g,%g,%g; nonfoveal-a=%g, direct-foveated=%g, sharpness=%g; readback-hr=0x%08x",
                        m.pid,eye,static_cast<unsigned long long>(m.present),static_cast<unsigned long long>(a.samples),m.width,m.height,m.resourceFormat,m.viewFormat,
                        double(m.uv[0]),double(m.uv[1]),double(m.uv[2]),double(m.uv[3]),double(m.nonFovealAlpha),double(m.directFoveated),double(m.sharpness),alphaError);
                    for(unsigned region=0;region<3&&used>0&&std::size_t(used)<sizeof alphaText;++region){
                        const auto v=a.alpha[region];const auto remaining=sizeof alphaText-std::size_t(used);
                        const auto n=std::snprintf(alphaText+used,remaining,"; region%u=%u/%u/%u, zero/partial=%u/%u",
                            region,unsigned(v&255),unsigned((v>>8)&255),unsigned((v>>16)&255),unsigned((v>>24)&65535),unsigned((v>>40)&65535));
                        if(n<0)break;
                        used+=n;
                    }
                    report(alphaText,stream_bridge::logging::Level::Debug);
                }
            }
            if(sourceReports.observe(stats,final)){
                using Source=stream_bridge::sender::SourceContractStatus;
                const auto at=[](Source why){return static_cast<unsigned>(why);};
                const auto count=[&](Source why){return static_cast<unsigned long long>(stats.sourceRefusals[at(why)]);};
                const auto declared=[&](Source why){return static_cast<unsigned>(stats.sourceProcessPairs[at(why)]>>32);};
                const auto native=[&](Source why){return static_cast<unsigned>(stats.sourceProcessPairs[at(why)]);};
                char sourceText[456]{};std::snprintf(sourceText,sizeof sourceText,
                    "coverage source refusals: no-declaration=%llu (native-pid=%u); foreign-process=%llu (declared/native-pid=%u/%u); revoked-generation=%llu (declared/native-pid=%u/%u); observed-causes=0x%x; approximate counters, not frame proof",
                    count(Source::NoDeclaration),native(Source::NoDeclaration),
                    count(Source::DifferentProcess),declared(Source::DifferentProcess),native(Source::DifferentProcess),
                    count(Source::RevokedGeneration),declared(Source::RevokedGeneration),native(Source::RevokedGeneration),stats.sourceCauses);
                report(sourceText,stream_bridge::logging::Level::Debug);
            }
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
            char text[344]{};std::snprintf(text,sizeof text,
                "coverage gates: device=%llu, context=%llu, draws=%llu/%llu, source=%llu, constants=%llu/%llu, eyes=%llu, pairs=%llu, copies=%llu/%llu, inputs=%llu, masks=%llu; last=%s",
                static_cast<unsigned long long>(stats.deviceOpened),static_cast<unsigned long long>(stats.contextsReady),
                static_cast<unsigned long long>(stats.nativeDraws),static_cast<unsigned long long>(stats.hookDraws),
                static_cast<unsigned long long>(stats.sourceFrames),static_cast<unsigned long long>(stats.constantCaptures),static_cast<unsigned long long>(stats.constantMaps),
                static_cast<unsigned long long>(stats.eyeDraws),static_cast<unsigned long long>(stats.completePairs),
                static_cast<unsigned long long>(stats.capturedCopies),static_cast<unsigned long long>(stats.nativeCopies),
                static_cast<unsigned long long>(stats.matchedInputs),static_cast<unsigned long long>(stats.certifiedMasks),
                stream_bridge::sender::coverageGateName(stats.lastRefusal));report(text,stream_bridge::logging::Level::Debug);
            std::snprintf(text,sizeof text,
                "coverage gate detail: %s=%llu, %s=%llu, %s=%llu; constant-bytes=%u; source=%ux%u/f%u, output=%ux%u/f%u; maps=%llu, inventory=%llu",
                stream_bridge::sender::coverageGateName(static_cast<stream_bridge::sender::CoverageGate>(top[0])),static_cast<unsigned long long>(stats.refusals[top[0]]),
                stream_bridge::sender::coverageGateName(static_cast<stream_bridge::sender::CoverageGate>(top[1])),static_cast<unsigned long long>(stats.refusals[top[1]]),
                stream_bridge::sender::coverageGateName(static_cast<stream_bridge::sender::CoverageGate>(top[2])),static_cast<unsigned long long>(stats.refusals[top[2]]),
                stats.lastConstantBytes,stats.lastSourceWidth,stats.lastSourceHeight,stats.lastSourceFormat,stats.lastOutputWidth,stats.lastOutputHeight,stats.lastOutputFormat,
                static_cast<unsigned long long>(stats.nativeMaps),static_cast<unsigned long long>(stats.inventoryRefreshes));report(text,stream_bridge::logging::Level::Debug);
            std::snprintf(text,sizeof text,
                "coverage readback: ready=%llu, busy=%llu, errors=%llu, last-hr=0x%08x; copy-age-us=%llu/max%llu, submit-age-us=%llu, read-us=%llu/max%llu",
                static_cast<unsigned long long>(stats.readbackReady),static_cast<unsigned long long>(stats.readbackBusy),static_cast<unsigned long long>(stats.readbackErrors),stats.lastReadbackResult,
                static_cast<unsigned long long>(stats.lastCopyAgeUs),static_cast<unsigned long long>(stats.maxCopyAgeUs),static_cast<unsigned long long>(stats.lastSubmissionAgeUs),
                static_cast<unsigned long long>(stats.lastReadbackUs),static_cast<unsigned long long>(stats.maxReadbackUs));report(text,stream_bridge::logging::Level::Debug);
            std::snprintf(text,sizeof text,
                "coverage completion: submitted=%llu, ready=%llu, pending=%llu, errors=%llu, last-hr=0x%08x; copy-age-us=%llu/max%llu",
                static_cast<unsigned long long>(stats.submittedReadbacks),static_cast<unsigned long long>(stats.queryReady),static_cast<unsigned long long>(stats.queryPending),static_cast<unsigned long long>(stats.queryErrors),stats.lastQueryResult,static_cast<unsigned long long>(stats.lastQueryAgeUs),static_cast<unsigned long long>(stats.maxQueryAgeUs));report(text,stream_bridge::logging::Level::Debug);
            const auto refused=[&](stream_bridge::sender::CoverageGate gate){return static_cast<unsigned long long>(stats.refusals[static_cast<unsigned>(gate)]);};
            std::snprintf(text,sizeof text,
                "coverage payload: masked-frames=%llu; original-video-bytes=%llu, rle-coverage-bytes=%llu, complete-au-bytes=%llu, max-complete-au-bytes=%llu; approximate counters, not frame identity",
                static_cast<unsigned long long>(stats.certifiedMasks),static_cast<unsigned long long>(stats.maskedVideoBytes),
                static_cast<unsigned long long>(stats.maskRleBytes),static_cast<unsigned long long>(stats.maskedWireBytes),static_cast<unsigned long long>(stats.maxMaskedWireBytes));report(text,stream_bridge::logging::Level::Debug);
            std::snprintf(text,sizeof text,
                "coverage encoding: raw-mask-bytes=%llu, encoded-coverage-bytes=%llu; frames raw/rle/lz4/zstd=%llu/%llu/%llu/%llu; encode-us=%llu/max%llu; lossless, approximate counters",
                static_cast<unsigned long long>(stats.maskRawBytes),static_cast<unsigned long long>(stats.maskEncodedBytes),
                static_cast<unsigned long long>(stats.maskEncodingFrames[1]),static_cast<unsigned long long>(stats.maskEncodingFrames[2]),static_cast<unsigned long long>(stats.maskEncodingFrames[3]),static_cast<unsigned long long>(stats.maskEncodingFrames[4]),
                static_cast<unsigned long long>(stats.lastEncodeUs),static_cast<unsigned long long>(stats.maxEncodeUs));report(text,stream_bridge::logging::Level::Debug);
            std::snprintf(text,sizeof text,
                "coverage lifecycle refusals: render-lock=%llu, copy-pair=%llu, stage-full=%llu, input=%llu, stage=%llu, session=%llu, source=%llu/%llu, rle=%llu, certificate=%llu",
                refused(stream_bridge::sender::CoverageGate::Lock),refused(stream_bridge::sender::CoverageGate::CopyPair),refused(stream_bridge::sender::CoverageGate::StageFull),
                refused(stream_bridge::sender::CoverageGate::InputReceipt),refused(stream_bridge::sender::CoverageGate::StageReceipt),refused(stream_bridge::sender::CoverageGate::CoverageSession),
                refused(stream_bridge::sender::CoverageGate::SourceContract),refused(stream_bridge::sender::CoverageGate::SourceRevoked),refused(stream_bridge::sender::CoverageGate::RleEncode),refused(stream_bridge::sender::CoverageGate::FrameCertificate));report(text,stream_bridge::logging::Level::Debug);
            // Public COM table metadata only; never query a new immediate
            // context here, since even a diagnostic getter may change dispatch.
            std::snprintf(text,sizeof text,
                "coverage context: hook-slots=0x%x, table-readable=%u, table-stable=%u, native-device-pointer=%u, facade-slots=%u, native-dispatch-mutations=%u",
                stats.contextHookSlots,unsigned(stats.contextTableReadable),unsigned(stats.contextTableStable),unsigned(stats.nativeDeviceSame),stats.contextFacadeSlots,stats.nativeDispatchMutations);report(text,stream_bridge::logging::Level::Debug);
        };
        std::uint32_t reportedFailures=0;
        unsigned recoveryReports=0;bool recoveringTransport=false;
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
            report(text,stream_bridge::logging::Level::Debug);
        };
        for(;;){
            {std::lock_guard<std::mutex> guard(mutex_);if(stopped_)break;}
            try{coverage_.tick();runtime_.tick();
                if(request_.explicitlyEnabled){stream_bridge::LocalConfig config;stream_bridge::LocalSendAuthority authority;stream_bridge::sender::SourceIpcState state;
                    const auto encoder=runtime_.encoder();
                    if(encoder.peerConfirmed&&encoder.probeObserved&&runtime_.control().peerVerified()&&runtime_.control().localConfig(config,authority)){state.binding=authority.channel.binding;state.generation=encoder.generation;state.compatible=true;}
                    sourceIpc_.tick(state);
                }
            }catch(...){sourceIpc_.stop();coverage_.stop();runtime_.stop();report("worker refused; ordinary video",stream_bridge::logging::Level::Warning);break;}
            const auto* coverageState=coverage_.stateName();if(request_.explicitlyEnabled&&coverageState!=lastCoverage){lastCoverage=coverageState;reportState(coverageState);}
            if(runtime_.state()!=last){
                using State=stream_bridge::sender::RuntimeState;
                if(last==State::Failed&&(runtime_.state()==State::Preparing||runtime_.state()==State::WaitingPeer)
                    &&runtime_.lastFailure().reason==stream_bridge::sender::RuntimeFailure::TransportLost){
                    recoveringTransport=true;
                    if(recoveryReports<8){report("sender recovery: fresh transport started; waiting for new authenticated video proof");++recoveryReports;}
                }
                if(recoveringTransport&&runtime_.state()==State::ProbeObserved){
                    recoveringTransport=false;
                    if(recoveryReports<8){report("sender recovery: fresh paired session and coded-AU proof confirmed");++recoveryReports;}
                }
                last=runtime_.state();reportState(stream_bridge::sender::runtimeStateName(last));
            }
            if(request_.explicitlyEnabled){reportReconfigure(false);reportWireBudget(false);reportCoverage(false);reportFailure();}
            std::unique_lock<std::mutex> guard(mutex_);changed_.wait_for(guard,std::chrono::milliseconds(8),[this]{return stopped_;});
        }
        report("sender worker stopping");
        report(stream_bridge::sender::runtimeStateName(runtime_.state()));
        report(coverage_.stateName());
        reportReconfigure(true);
        reportWireBudget(true);
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
