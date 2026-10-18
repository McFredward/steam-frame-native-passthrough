// Public OpenVR transport diagnostic. Never launched by the product or installer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <conio.h>
#include <openvr_capi.h>
#include "projection-overlay-probe-core.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
std::atomic<bool> stopRequested{false};
BOOL WINAPI consoleControl(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stopRequested = true;
        return TRUE;
    }
    return FALSE;
}
bool runningServer() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile,L"vrserver.exe") == 0) { found = true; break; }
    CloseHandle(snapshot);
    return found;
}
template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

class PublicApi final : public projection_probe::Api {
public:
    explicit PublicApi(const wchar_t* dll) : path_(dll) {}
    ~PublicApi() override { shutdown(); }
    bool initialize() override {
        if (!runningServer()) return fail("existing vrserver.exe required; refusing runtime autostart");
        module_ = LoadLibraryExW(path_.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return fail("LoadLibraryExW failed",GetLastError());
        init_ = reinterpret_cast<Init>(GetProcAddress(module_,"VR_InitInternal"));
        get_ = reinterpret_cast<Get>(GetProcAddress(module_,"VR_GetGenericInterface"));
        shutdown_ = reinterpret_cast<Shutdown>(GetProcAddress(module_,"VR_ShutdownInternal"));
        if (!init_ || !get_ || !shutdown_) return fail("public OpenVR exports missing");
        EVRInitError error = EVRInitError_VRInitError_None;
        init_(&error, EVRApplicationType_VRApplication_Overlay);
        initialized_ = error == EVRInitError_VRInitError_None;
        if (!initialized_) return fail("VR_InitInternal Overlay failed",error);
        std::string name = std::string("FnTable:") + IVRSystem_Version;
        system_ = reinterpret_cast<VR_IVRSystem_FnTable*>(get_(name.c_str(),&error));
        if (error || !system_) return fail("public IVRSystem table refused",error);
        name = std::string("FnTable:") + IVROverlay_Version;
        overlay_ = reinterpret_cast<VR_IVROverlay_FnTable*>(get_(name.c_str(),&error));
        if (error || !overlay_) return fail("public IVROverlay table refused",error);
        TrackedDevicePose_t pose{};
        system_->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding,0,&pose,1);
        if (!pose.bDeviceIsConnected || !pose.bPoseIsValid) return fail("current HMD pose invalid");
        static_assert(sizeof(head_) == sizeof(pose.mDeviceToAbsoluteTracking),"matrix ABI");
        for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 4; ++c)
            head_.m[r][c] = pose.mDeviceToAbsoluteTracking.m[r][c];
        if (!projection_probe::finite(head_)) return fail("HMD matrix nonfinite");
        int32_t adapterIndex = -1;
        system_->GetDXGIOutputInfo(&adapterIndex);
        if (adapterIndex < 0) return fail("HMD DXGI adapter unavailable");
        IDXGIFactory1* factory = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory));
        if (SUCCEEDED(hr)) hr = factory->EnumAdapters1(static_cast<UINT>(adapterIndex),&adapter);
        release(factory);
        if (FAILED(hr)) { release(adapter); return fail("DXGI adapter lookup failed",hr); }
        hr = D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device_,nullptr,&context_);
        release(adapter);
        if (FAILED(hr)) return fail("D3D11 device failed",hr);
        std::printf("SDK interfaces %s / %s; DXGI adapter %d; static RGBA8 %ux%u per eye\n",
            IVRSystem_Version,IVROverlay_Version,adapterIndex,
            projection_probe::PatternSize,projection_probe::PatternSize);
        return true;
    }
    bool create(unsigned eye, projection_probe::Handle& handle) override {
        char key[100], title[100];
        std::snprintf(key,sizeof(key),"native-passthrough.projection-probe.%lu.%u",GetCurrentProcessId(),eye);
        std::snprintf(title,sizeof(title),"RGBA transport probe %s",eye == 0 ? "LEFT" : "RIGHT");
        VROverlayHandle_t native = 0;
        const auto error = overlay_->CreateOverlay(key,title,&native);
        handle = native;
        std::printf("eye=%u overlay=%llu create_error=%d\n",eye,static_cast<unsigned long long>(native),error);
        return check(error,"CreateOverlay");
    }
    bool configure(unsigned eye, projection_probe::Handle handle) override {
        const auto nativeEye = eye == 0 ? EVREye_Eye_Left : EVREye_Eye_Right;
        const auto relative = system_->GetEyeToHeadTransform(nativeEye);
        projection_probe::Matrix eyeToHead{};
        for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 4; ++c)
            eyeToHead.m[r][c] = relative.m[r][c];
        auto absolute = projection_probe::compose(head_,eyeToHead);
        if (!projection_probe::finite(absolute)) return fail("eye matrix nonfinite");
        HmdMatrix34_t nativePose{};
        std::memcpy(&nativePose,&absolute,sizeof(nativePose));
        VROverlayProjection_t projection{};
        system_->GetProjectionRaw(nativeEye,&projection.fLeft,&projection.fRight,&projection.fTop,&projection.fBottom);
        if (!std::isfinite(projection.fLeft) || !std::isfinite(projection.fRight) ||
            !std::isfinite(projection.fTop) || !std::isfinite(projection.fBottom) ||
            projection.fLeft >= projection.fRight || projection.fTop >= projection.fBottom)
            return fail("invalid public projection frustum");
        std::printf("eye=%u frustum=%g,%g,%g,%g frozen_standing_eye_pose=",eye,
            projection.fLeft,projection.fRight,projection.fTop,projection.fBottom);
        for (const auto& row : absolute.m) for (float n : row) std::printf("%g,",n);
        std::printf("\n");
        return check(overlay_->SetOverlayTransformProjection(handle,ETrackingUniverseOrigin_TrackingUniverseStanding,
                &nativePose,&projection,nativeEye),"SetOverlayTransformProjection") &&
            check(overlay_->SetOverlayInputMethod(handle,VROverlayInputMethod_None),"SetOverlayInputMethod") &&
            check(overlay_->SetOverlayFlag(handle,VROverlayFlags_IsPremultiplied,true),"IsPremultiplied") &&
            check(overlay_->SetOverlayFlag(handle,VROverlayFlags_IgnoreTextureAlpha,false),"IgnoreTextureAlpha=false") &&
            check(overlay_->SetOverlayFlag(handle,VROverlayFlags_VisibleInDashboard,false),"VisibleInDashboard=false") &&
            check(overlay_->SetOverlayColor(handle,1,1,1),"SetOverlayColor") &&
            check(overlay_->SetOverlayAlpha(handle,1),"SetOverlayAlpha") &&
            check(overlay_->SetOverlayTextureColorSpace(handle,EColorSpace_ColorSpace_Gamma),"SetOverlayTextureColorSpace");
    }
    bool texture(unsigned eye, projection_probe::Handle handle) override {
        const auto pixels = projection_probe::pattern(eye);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = projection_probe::PatternSize;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = projection_probe::PatternSize * sizeof(projection_probe::Pixel);
        const HRESULT hr = device_->CreateTexture2D(&desc,&data,&textures_[eye]);
        if (FAILED(hr)) return fail("immutable RGBA8 texture creation failed",hr);
        Texture_t texture{textures_[eye],ETextureType_TextureType_DirectX,EColorSpace_ColorSpace_Gamma};
        return check(overlay_->SetOverlayTexture(handle,&texture),"SetOverlayTexture");
    }
    bool show(projection_probe::Handle h) override { return check(overlay_->ShowOverlay(h),"ShowOverlay"); }
    bool hide(projection_probe::Handle h) override { return check(overlay_->HideOverlay(h),"HideOverlay"); }
    bool clear(projection_probe::Handle h) override { return check(overlay_->ClearOverlayTexture(h),"ClearOverlayTexture"); }
    bool destroy(projection_probe::Handle h) override { return check(overlay_->DestroyOverlay(h),"DestroyOverlay"); }
    bool quitting() {
        VREvent_t event{};
        // Observe runtime quit without consuming another application's input.
        while (system_ && system_->PollNextEvent(&event,sizeof(event)))
            if (event.eventType == EVREventType_VREvent_Quit) return true;
        return false;
    }
    void shutdown() override {
        if (initialized_ && shutdown_) { shutdown_(); initialized_ = false; }
        for (auto& texture : textures_) release(texture);
        release(context_); release(device_);
        system_ = nullptr; overlay_ = nullptr;
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
        init_ = nullptr; get_ = nullptr; shutdown_ = nullptr;
    }
private:
    static bool fail(const char* what, long error = 0) {
        std::fprintf(stderr,"REFUSED %s (code=%ld)\n",what,error); return false;
    }
    static bool check(EVROverlayError error, const char* call) {
        return error == EVROverlayError_VROverlayError_None || fail(call,error);
    }
    using Init = intptr_t (__cdecl*)(EVRInitError*,EVRApplicationType);
    using Get = intptr_t (__cdecl*)(const char*,EVRInitError*);
    using Shutdown = void (__cdecl*)();
    std::wstring path_;
    HMODULE module_ = nullptr;
    Init init_ = nullptr; Get get_ = nullptr; Shutdown shutdown_ = nullptr;
    bool initialized_ = false;
    VR_IVRSystem_FnTable* system_ = nullptr;
    VR_IVROverlay_FnTable* overlay_ = nullptr;
    projection_probe::Matrix head_{};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::array<ID3D11Texture2D*,2> textures_{};
};
} // namespace

int wmain(int argc,wchar_t** argv) {
    if (argc != 4 || std::wcscmp(argv[1],L"--probe") || std::wcscmp(argv[2],L"--openvr") ||
        !((std::wcslen(argv[3]) > 3 && argv[3][1] == L':' && (argv[3][2] == L'\\' || argv[3][2] == L'/')) ||
          (argv[3][0] == L'\\' && argv[3][1] == L'\\'))) {
        std::fprintf(stderr,"Explicit diagnostic only. Usage: projection-overlay-probe.exe --probe --openvr <absolute openvr_api.dll>\n"
            "Start SteamVR and the PC-VR stream first. Press Enter/Escape/Ctrl+C to remove both temporary overlays.\n"
            "No product setting, autostart, timer, receiver install or Steam configuration change.\n");
        return 2;
    }
    if (GetFileAttributesW(argv[3]) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr,"OpenVR DLL path does not exist.\n"); return 2;
    }
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    std::printf("projection-overlay-probe-v1 UTC=%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu\n",
        utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,GetCurrentProcessId());
    std::setvbuf(stdout,nullptr,_IONBF,0);
    SetConsoleCtrlHandler(consoleControl,TRUE);
    PublicApi api(argv[3]);
    projection_probe::Session session(api);
    if (!session.start(true)) return 1;
    std::printf("VISIBLE both public projection overlays. Enter/Escape/Ctrl+C removes them; no automatic timeout.\n");
    while (!stopRequested && !api.quitting()) {
        if (_kbhit()) { const int key = _getch(); if (key == 13 || key == 27) break; }
        Sleep(25);
    }
    const bool clean = session.stop();
    std::printf("STOPPED both overlays cleared/destroyed; cleanup_ok=%s\n",clean ? "true" : "false");
    SetConsoleCtrlHandler(consoleControl,FALSE);
    return clean ? 0 : 1;
}
