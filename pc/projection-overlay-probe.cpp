// Explicit static mono world-quad delivery preview; no Steam/Frame settings changes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#include <openvr_capi.h>
#include "projection-overlay-probe-core.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
namespace {
std::atomic<bool> stopRequested{false};
BOOL WINAPI consoleControl(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stopRequested.store(true); return TRUE;
    }
    return FALSE;
}
template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
class Stdout final : public projection_probe::LogSink {
public:
    bool write(const std::string& line) override {
        return std::fwrite(line.data(),1,line.size(),stdout) == line.size() && std::fflush(stdout) == 0;
    }
};
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

class PublicApi final : public projection_probe::Api {
public:
    PublicApi(const wchar_t* dll, projection_probe::Log& log) : path_(dll), log_(log) {}
    ~PublicApi() override { shutdown(); }
    bool initialize() override {
        file_ = checkedLibrary(path_.c_str());
        if (file_ == INVALID_HANDLE_VALUE) return fail("library-path-or-pe",GetLastError());
        module_ = LoadLibraryExW(path_.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return fail("library-load",GetLastError());
        if (!exported(init_,"VR_InitInternal2") || !exported(get_,"VR_GetGenericInterface")
            || !exported(shutdown_,"VR_ShutdownInternal")) return fail("exports");
        EVRInitError error = EVRInitError_VRInitError_None;
        // Background never starts/keeps alive the runtime. No Overlay/Scene fallback.
        init_(&error,EVRApplicationType_VRApplication_Background,nullptr);
        if (error != EVRInitError_VRInitError_None) return fail("background-init",error);
        initialized_ = true;
        system_ = static_cast<VR_IVRSystem_FnTable*>(get_("FnTable:IVRSystem_026",&error));
        if (error || !system_) return fail("system026",error);
        overlay_ = static_cast<VR_IVROverlay_FnTable*>(get_("FnTable:IVROverlay_028",&error));
        if (error || !overlay_) return fail("overlay028",error);
        if (!system_->GetDeviceToAbsoluteTrackingPose || !system_->GetDXGIOutputInfo
            || !system_->GetRecommendedRenderTargetSize
            || !system_->PollNextEvent || !overlay_->CreateOverlay || !overlay_->DestroyOverlay
            || !overlay_->SetOverlayTransformAbsolute || !overlay_->SetOverlayWidthInMeters || !overlay_->SetOverlayTexelAspect
            || !overlay_->GetOverlayWidthInMeters || !overlay_->GetOverlayTexelAspect || !overlay_->SetOverlayInputMethod || !overlay_->SetOverlayFlag
            || !overlay_->SetOverlayColor || !overlay_->SetOverlayAlpha || !overlay_->SetOverlayTextureColorSpace
            || !overlay_->SetOverlayTexture || !overlay_->ClearOverlayTexture || !overlay_->ShowOverlay
            || !overlay_->HideOverlay || !overlay_->IsDashboardVisible || !overlay_->IsOverlayVisible || !overlay_->GetOverlayTextureSize) return fail("required-getters-or-overlay-functions");
        TrackedDevicePose_t pose{};
        system_->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding,0,&pose,1);
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid) return fail("hmd-pose");
        static_assert(sizeof(head_) == sizeof(pose.mDeviceToAbsoluteTracking),"matrix ABI");
        for (unsigned r=0; r<3; ++r) for (unsigned c=0; c<4; ++c) head_.m[r][c]=pose.mDeviceToAbsoluteTracking.m[r][c];
        if (!projection_probe::finite(head_)) return fail("finite-hmd-pose");
        std::uint32_t width = 0, height = 0;
        system_->GetRecommendedRenderTargetSize(&width,&height);
        if (!width || !height) return fail("recommended-eye-size");
        int32_t adapterIndex = -1; system_->GetDXGIOutputInfo(&adapterIndex);
        if (adapterIndex < 0) return fail("dxgi-adapter");
        IDXGIFactory1* factory = nullptr; IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory));
        if (SUCCEEDED(hr)) hr = factory->EnumAdapters1(static_cast<UINT>(adapterIndex),&adapter);
        release(factory);
        if (FAILED(hr)) { release(adapter); return fail("dxgi-lookup",hr); }
        hr = D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device_,nullptr,&context_);
        release(adapter);
        if (FAILED(hr)) return fail("d3d11-device",hr);
        return line("SFNP_PROJECTION_META_V2 system=026 overlay=028 application=Background recommended_eye=%u,%u pattern=%u texture=%u,%u adapter=%d geometry=common_world_mono_quad distance_m=1.2 width_m=0.6 hold_head_still=1 local_only=1\n",
            width,height,projection_probe::PatternSize,projection_probe::PatternSize,projection_probe::PatternSize,adapterIndex);
    }
    bool create(projection_probe::Handle& handle) override {
        char key[100], title[]="Mono quad delivery preview";
        std::snprintf(key,sizeof(key),"native-passthrough.quad-preview.%lu",GetCurrentProcessId());
        VROverlayHandle_t native = 0;
        const auto error = overlay_->CreateOverlay(key,title,&native); handle = native;
        return check(error,"create-overlay");
    }
    bool configure(projection_probe::Handle handle) override {
        const auto pose=projection_probe::quadPose(head_);
        if (!projection_probe::finite(pose)) return fail("finite-common-quad-pose");
        HmdMatrix34_t nativePose{}; std::memcpy(&nativePose,&pose,sizeof(nativePose));
        if (!line("SFNP_PROJECTION_GEOMETRY_V2 mode=common_world_mono_quad intended_width_m=0.6 intended_height_m=0.6 distance_m=1.2 frozen_pose=%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g local_only=1\n",
            pose.m[0][0],pose.m[0][1],pose.m[0][2],pose.m[0][3],
            pose.m[1][0],pose.m[1][1],pose.m[1][2],pose.m[1][3],
            pose.m[2][0],pose.m[2][1],pose.m[2][2],pose.m[2][3])) return false;
        return check(overlay_->SetOverlayTransformAbsolute(handle,ETrackingUniverseOrigin_TrackingUniverseStanding,&nativePose),"absolute-common-quad")
            && check(overlay_->SetOverlayWidthInMeters(handle,projection_probe::QuadWidth),"quad-width")
            && check(overlay_->SetOverlayTexelAspect(handle,1),"initial-texel-aspect")
            && check(overlay_->SetOverlayFlag(handle,VROverlayFlags_SideBySide_Parallel,false),"mono-not-stereo-parallel")
            && check(overlay_->SetOverlayFlag(handle,VROverlayFlags_SideBySide_Crossed,false),"not-stereo-crossed")
            && check(overlay_->SetOverlayInputMethod(handle,VROverlayInputMethod_None),"input-none")
            && check(overlay_->SetOverlayFlag(handle,VROverlayFlags_IsPremultiplied,true),"premultiplied")
            && check(overlay_->SetOverlayFlag(handle,VROverlayFlags_IgnoreTextureAlpha,false),"preserve-alpha")
            && check(overlay_->SetOverlayFlag(handle,VROverlayFlags_VisibleInDashboard,false),"hide-in-dashboard")
            && check(overlay_->SetOverlayColor(handle,1,1,1),"overlay-color")
            && check(overlay_->SetOverlayAlpha(handle,0),"prepare-alpha-zero")
            && check(overlay_->SetOverlayTextureColorSpace(handle,EColorSpace_ColorSpace_Gamma),"texture-gamma");
    }
    bool texture(projection_probe::Handle) override {
        pixels_ = projection_probe::pattern(0);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = projection_probe::PatternSize; desc.Height = projection_probe::PatternSize;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = pixels_.data();
        data.SysMemPitch = projection_probe::PatternSize*sizeof(projection_probe::Pixel);
        const HRESULT hr = device_->CreateTexture2D(&desc,&data,&texture_);
        return !FAILED(hr) || fail("immutable-mono-rgba8",hr);
    }
    bool submit(projection_probe::Handle handle) override {
        Texture_t texture{texture_,ETextureType_TextureType_DirectX,EColorSpace_ColorSpace_Gamma};
        return check(overlay_->SetOverlayTexture(handle,&texture),"submit-overlay-texture");
    }

    bool flush() override {
        // No swapchain/Present exists in this one-upload helper. Explicitly send
        // queued producer upload BEFORE handing the texture to the compositor.
        // Flush is asynchronous: this is not GPU completion or remote delivery.
        context_->Flush();
        const HRESULT hr=device_->GetDeviceRemovedReason();
        return !FAILED(hr) || fail("producer-device-removed",hr);
    }
    bool show(projection_probe::Handle h) override { return check(overlay_->ShowOverlay(h),"show-overlay"); }
    bool reveal(projection_probe::Handle h) override { return check(overlay_->SetOverlayAlpha(h,1),"reveal-alpha-one"); }
    bool verify(projection_probe::Handle h) override {
        std::uint32_t width=0,height=0; float meters=0,aspect=0;
        if (!check(overlay_->GetOverlayTextureSize(h,&width,&height),"local-texture-extent")
            || !check(overlay_->GetOverlayWidthInMeters(h,&meters),"local-width")
            || !check(overlay_->GetOverlayTexelAspect(h,&aspect),"local-texel-aspect")) return false;
        if (!std::isfinite(meters) || !std::isfinite(aspect)) return fail("nonfinite-local-geometry");
        const bool visible=overlay_->IsOverlayVisible(h), dashboard=overlay_->IsDashboardVisible();
        if (!line("SFNP_PROJECTION_LOCAL_V2 overlay_visible=%u dashboard_visible=%u texture_width=%u texture_height=%u width_m=%g texel_aspect=%g producer_flushed=1 gpu_completion_proven=0 remote_visibility_proven=0\n",
            visible?1u:0u,dashboard?1u:0u,width,height,meters,aspect)) return false;
        return (width==projection_probe::PatternSize && height==projection_probe::PatternSize
            && std::abs(meters-projection_probe::QuadWidth)<.002f && std::abs(aspect-1)<.002f) || fail("local-mono-extent-or-geometry-mismatch");
    }
    bool ready() override { return log_.emit(projection_probe::ReadyLine); }
    void stopped(bool clean) override {
        if (clean) log_.emit(projection_probe::StoppedLine);
        else fail("overlay-cleanup");
    }
    bool hide(projection_probe::Handle h) override { return check(overlay_->HideOverlay(h),"hide-overlay"); }
    bool clear(projection_probe::Handle h) override { return check(overlay_->ClearOverlayTexture(h),"clear-texture"); }
    bool destroy(projection_probe::Handle h) override { return check(overlay_->DestroyOverlay(h),"destroy-overlay"); }
    bool quitting() {
        VREvent_t event{};
        for (unsigned i = 0; i < 16 && system_ && system_->PollNextEvent(&event,sizeof(event)); ++i)
            if (event.eventType == EVREventType_VREvent_Quit) return true;
        return false;
    }
    void shutdown() override {
        if (initialized_ && shutdown_) { initialized_ = false; shutdown_(); }
        release(texture_); std::vector<projection_probe::Pixel>().swap(pixels_);
        release(context_); release(device_); system_ = nullptr; overlay_ = nullptr;
        init_ = nullptr; get_ = nullptr; shutdown_ = nullptr;
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    }
private:
    bool line(const char* format, ...) {
        char buffer[1024]; va_list args; va_start(args,format);
        const int n = std::vsnprintf(buffer,sizeof(buffer),format,args); va_end(args);
        return n > 0 && n < static_cast<int>(sizeof(buffer)) && log_.emit(std::string(buffer,static_cast<std::size_t>(n)));
    }
    bool fail(const char* stage, long error = 0) { line("SFNP_PROJECTION_REFUSED_V2 stage=%s code=%ld\n",stage,error); return false; }
    bool check(EVROverlayError error, const char* call) { return error == EVROverlayError_VROverlayError_None || fail(call,error); }
    template<class T> bool exported(T& value, const char* name) {
        FARPROC address = GetProcAddress(module_,name); static_assert(sizeof(address) == sizeof(value),"export ABI");
        std::memcpy(&value,&address,sizeof(value)); return value != nullptr;
    }
    using Init = std::uint32_t (__cdecl*)(EVRInitError*,EVRApplicationType,const char*);
    using Get = void* (__cdecl*)(const char*,EVRInitError*);
    using Shutdown = void (__cdecl*)();
    std::wstring path_; projection_probe::Log& log_; HANDLE file_ = INVALID_HANDLE_VALUE;
    HMODULE module_ = nullptr; Init init_ = nullptr; Get get_ = nullptr; Shutdown shutdown_ = nullptr;
    bool initialized_ = false; VR_IVRSystem_FnTable* system_ = nullptr; VR_IVROverlay_FnTable* overlay_ = nullptr;
    projection_probe::Matrix head_{}; ID3D11Device* device_ = nullptr; ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr; std::vector<projection_probe::Pixel> pixels_;
};
bool pipeStop(HANDLE input) {
    DWORD available = 0;
    if (!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr)) return true;
    if (!available) return false;
    unsigned char byte = 0; DWORD read = 0; ReadFile(input,&byte,1,&read,nullptr); return true;
}
bool consoleStop() {
    if (!_kbhit()) return false;
    const auto key = _getwch(); return key == 13 || key == 27;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (_setmode(_fileno(stdout),_O_BINARY) == -1) return 2;
    Stdout sink; projection_probe::Log log(sink);
    if (argc != 4 || std::wcscmp(argv[2],L"--openvr")
        || (std::wcscmp(argv[1],L"--stdin-pipe") && std::wcscmp(argv[1],L"--probe"))) {
        log.emit("SFNP_PROJECTION_REFUSED_V2 stage=arguments code=0\n"); return 2;
    }
    const bool piped = std::wcscmp(argv[1],L"--stdin-pipe") == 0;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if ((piped && GetFileType(input) != FILE_TYPE_PIPE) || (!piped && !_isatty(_fileno(stdin)))) {
        log.emit("SFNP_PROJECTION_REFUSED_V2 stage=stdin code=0\n"); return 2;
    }
    if (piped && pipeStop(input)) {
        log.emit("SFNP_PROJECTION_REFUSED_V2 stage=cancelled-before-start code=0\n"); return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDllDirectoryW(L""); SetConsoleCtrlHandler(consoleControl,TRUE);
    bool clean = false, ready = false;
    try {
        PublicApi api(argv[3],log); projection_probe::Session session(api);
        if (!session.start(true)) return 1;
        ready = true;
        while (!stopRequested.load() && !api.quitting()) {
            if (piped ? pipeStop(input) : consoleStop()) break;
            Sleep(25); // Poll cadence only; no automatic timeout or expiry.
        }
        clean = session.stop();
    } catch (...) {
        if (!ready) log.emit("SFNP_PROJECTION_REFUSED_V2 stage=exception code=0\n");
        return 1;
    }
    SetConsoleCtrlHandler(consoleControl,FALSE);
    return clean && !log.failed() ? 0 : 1;
}
