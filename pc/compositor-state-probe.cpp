// Public OpenVR reads only. No scene/overlay creation, setters or autostart.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#include <openvr_capi.h>
#include "compositor-state-probe-core.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
using namespace compositor_probe;
std::atomic<bool> stopping{false};
BOOL WINAPI control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stopping.store(true); return TRUE;
    }
    return FALSE;
}
class Stdout final : public Sink {
public:
    bool write(const std::string& line) override {
        return std::fwrite(line.data(),1,line.size(),stdout) == line.size() && std::fflush(stdout) == 0;
    }
};
// Keep the checked file open for this client lifetime. Absolute, canonical PE64
// paths only; Wine's Z:\ native Steam installation mapping remains valid.
HANDLE checkedLibrary(const wchar_t* path) {
    const auto length = std::wcslen(path);
    if (length < 4 || length >= 1024 || path[1] != L':' || path[2] != L'\\'
        || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return INVALID_HANDLE_VALUE;
    wchar_t canonical[1024];
    const DWORD size = GetFullPathNameW(path,1024,canonical,nullptr);
    if (!size || size >= 1024 || _wcsicmp(path,canonical)
        || _wcsicmp(std::wcsrchr(path,L'\\') + 1,L"openvr_api.dll")) return INVALID_HANDLE_VALUE;
    for (std::size_t i = 3; i < length; ++i) if (canonical[i] == L'\\') {
        canonical[i] = 0; const DWORD flags = GetFileAttributesW(canonical); canonical[i] = L'\\';
        if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return INVALID_HANDLE_VALUE;
    }
    const DWORD flags = GetFileAttributesW(path);
    if (flags == INVALID_FILE_ATTRIBUTES || (flags & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        return INVALID_HANDLE_VALUE;
    HANDLE file = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return file;
    LARGE_INTEGER bytes{}; IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 pe{}; DWORD read = 0;
    bool valid = GetFileSizeEx(file,&bytes) && bytes.QuadPart >= 512 && bytes.QuadPart <= 8 * 1024 * 1024
        && ReadFile(file,&dos,sizeof(dos),&read,nullptr) && read == sizeof(dos)
        && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 64
        && dos.e_lfanew <= bytes.QuadPart - static_cast<LONGLONG>(sizeof(pe));
    if (valid) valid = SetFilePointer(file,dos.e_lfanew,nullptr,FILE_BEGIN) != INVALID_SET_FILE_POINTER
        && ReadFile(file,&pe,sizeof(pe),&read,nullptr) && read == sizeof(pe)
        && pe.Signature == IMAGE_NT_SIGNATURE && pe.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
        && pe.FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)
        && pe.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && (pe.FileHeader.Characteristics & IMAGE_FILE_DLL);
    if (!valid) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
    return file;
}
class PublicApi final : public Api {
public:
    explicit PublicApi(const wchar_t* path) : path_(path) {}
    ~PublicApi() override { shutdown(); }
    bool initialize(Status& status) override {
        status.stage = Stage::Library;
        file_ = checkedLibrary(path_.c_str());
        if (file_ == INVALID_HANDLE_VALUE) { status.code = static_cast<std::int32_t>(GetLastError()); return false; }
        module_ = LoadLibraryExW(path_.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) { status.code = static_cast<std::int32_t>(GetLastError()); return false; }
        status.stage = Stage::Exports;
        if (!exported(init_,"VR_InitInternal2") || !exported(get_,"VR_GetGenericInterface")
            || !exported(shutdown_,"VR_ShutdownInternal")) return false;
        // Background=3 cannot start/keep SteamVR alive. No vrserver.exe process
        // census: Proton may use native Linux vrserver through its SDK bridge.
        EVRInitError error = EVRInitError_VRInitError_None;
        init_(&error,EVRApplicationType_VRApplication_Background,nullptr);
        status.stage = Stage::Background; status.code = error;
        if (error != EVRInitError_VRInitError_None) return false;
        initialized_ = true;
        if (!table(system_,"FnTable:IVRSystem_026",Stage::System,status)
            || !table(compositor_,"FnTable:IVRCompositor_029",Stage::Compositor,status)
            || !table(overlay_,"FnTable:IVROverlay_028",Stage::Overlay,status)
            || !table(settings_,"FnTable:IVRSettings_003",Stage::Settings,status)) return false;
        status = {Stage::Functions,0};
        return system_->IsTrackedDeviceConnected && system_->GetTrackedDeviceClass && system_->GetStringTrackedDeviceProperty
            && compositor_->GetCurrentFadeColor && compositor_->GetCurrentGridAlpha && compositor_->CanRenderScene
            && compositor_->GetCurrentSceneFocusProcess && compositor_->GetLastFrameRenderer
            && overlay_->IsDashboardVisible && settings_->GetBool && settings_->GetFloat;
    }
    std::uint64_t nowMs() override { return GetTickCount64(); }
    Color fade(bool background) override {
        const auto c = compositor_->GetCurrentFadeColor(background); return {c.r,c.g,c.b,c.a};
    }
    float gridAlpha() override { return compositor_->GetCurrentGridAlpha(); }
    bool dashboardVisible() override { return overlay_->IsDashboardVisible(); }
    bool canRenderScene() override { return compositor_->CanRenderScene(); }
    std::uint32_t sceneFocus() override { return compositor_->GetCurrentSceneFocusProcess(); }
    std::uint32_t lastRenderer() override { return compositor_->GetLastFrameRenderer(); }
    Value<bool> boolean(BoolSetting setting) override {
        char section[] = "steamvr", dashboard[] = "dashboard";
        char noGrid[] = "doNotFadeToGrid", tracking[] = "forceFadeOnBadTracking", standalone[] = "standaloneSystemLayer";
        EVRSettingsError error = EVRSettingsError_VRSettingsError_None;
        const bool value = settings_->GetBool(setting == BoolSetting::StandaloneSystemLayer ? dashboard : section,
            setting == BoolSetting::DoNotFadeToGrid ? noGrid : setting == BoolSetting::ForceFadeOnBadTracking ? tracking : standalone,&error);
        return {value,static_cast<std::int32_t>(error)};
    }
    Value<float> sceneDimming() override {
        char section[] = "steamvr", key[] = "envBrightnessSceneAppDimming";
        EVRSettingsError error = EVRSettingsError_VRSettingsError_None;
        const float value = settings_->GetFloat(section,key,&error); return {value,static_cast<std::int32_t>(error)};
    }
    bool connected() override { return system_->IsTrackedDeviceConnected(0); }
    std::int32_t deviceClass() override { return system_->GetTrackedDeviceClass(0); }
    Text property(Property property) override {
        const auto id = property == Property::Tracking ? ETrackedDeviceProperty_Prop_TrackingSystemName_String
            : property == Property::Model ? ETrackedDeviceProperty_Prop_ModelNumber_String
            : ETrackedDeviceProperty_Prop_RegisteredDeviceType_String;
        std::array<char,512> buffer; buffer.fill(static_cast<char>(0xff));
        ETrackedPropertyError error = ETrackedPropertyError_TrackedProp_Success;
        const auto size = system_->GetStringTrackedDeviceProperty(0,id,buffer.data(),static_cast<std::uint32_t>(buffer.size()),&error);
        Text result; result.error = error; result.valid = false;
        if (error || size < 1 || size > 97 || buffer[size - 1] != 0 || std::memchr(buffer.data(),0,size - 1)) return result;
        result.value.assign(buffer.data(),size - 1); result.valid = true;
        result.valid = bounded(result); return result;
    }
    void shutdown() override {
        if (initialized_ && shutdown_) { initialized_ = false; shutdown_(); }
        system_ = nullptr; compositor_ = nullptr; overlay_ = nullptr; settings_ = nullptr;
        init_ = nullptr; get_ = nullptr; shutdown_ = nullptr;
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    }
private:
    template<class T> bool exported(T& value, const char* name) {
        FARPROC address = GetProcAddress(module_,name); static_assert(sizeof(address) == sizeof(value),"export pointer ABI");
        std::memcpy(&value,&address,sizeof(value)); return value != nullptr;
    }
    template<class T> bool table(T*& value, const char* name, Stage stage, Status& status) {
        EVRInitError error = EVRInitError_VRInitError_None;
        value = static_cast<T*>(get_(name,&error)); status = {stage,static_cast<std::int32_t>(error)};
        return error == EVRInitError_VRInitError_None && value;
    }
    using Init = std::uint32_t (__cdecl*)(EVRInitError*,EVRApplicationType,const char*);
    using Get = void* (__cdecl*)(const char*,EVRInitError*);
    using Shutdown = void (__cdecl*)();
    std::wstring path_; HMODULE module_ = nullptr; HANDLE file_ = INVALID_HANDLE_VALUE;
    Init init_ = nullptr; Get get_ = nullptr; Shutdown shutdown_ = nullptr; bool initialized_ = false;
    VR_IVRSystem_FnTable* system_ = nullptr; VR_IVRCompositor_FnTable* compositor_ = nullptr;
    VR_IVROverlay_FnTable* overlay_ = nullptr; VR_IVRSettings_FnTable* settings_ = nullptr;
};
bool pipeStop(HANDLE input) {
    DWORD available = 0;
    if (!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr)) return true;
    if (!available) return false;
    // This is the sole reader. Peek proved at least this byte is available;
    // EOF/error also means stop. There is no blocked read or reader thread.
    unsigned char stop = 0; DWORD read = 0;
    ReadFile(input,&stop,1,&read,nullptr); return true;
}
bool consoleStop() {
    if (!_kbhit()) return false;
    const auto key = _getwch(); return key == 13 || key == 27;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (_setmode(_fileno(stdout),_O_BINARY) == -1) return 2;
    Stdout sink; Output output(sink);
    if (argc != 4 || std::wcscmp(argv[2],L"--openvr")
        || (std::wcscmp(argv[1],L"--stdin-pipe") && std::wcscmp(argv[1],L"--observe"))) {
        refusal(output,{Stage::Arguments,0}); return 2;
    }
    const bool piped = std::wcscmp(argv[1],L"--stdin-pipe") == 0;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if ((piped && GetFileType(input) != FILE_TYPE_PIPE) || (!piped && !_isatty(_fileno(stdin)))) {
        refusal(output,{Stage::Stdin,0}); return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDllDirectoryW(L""); SetConsoleCtrlHandler(control,TRUE);
    bool ready = false;
    try {
        PublicApi api(argv[3]); Monitor monitor(api,output);
        if (!monitor.start(true)) return 2;
        ready = true;
        while (!stopping.load()) {
            if (piped ? pipeStop(input) : consoleStop()) break;
            monitor.tick();
            // Poll cadence only; there is no elapsed-time stop/expiry/restart.
            // After the log budget the monitor remains idle until manual stop/EOF.
            Sleep(100);
        }
        monitor.stop();
    } catch (...) {
        // After ready, RAII already emitted the one stopped terminal. Never
        // follow it with a second terminal, even on an allocation exception.
        if (!ready) refusal(output,{Stage::Exception,0});
        return 2;
    }
    return output.failed() ? 2 : 0;
}
