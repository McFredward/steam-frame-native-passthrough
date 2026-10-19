# Shared-allocation admission regression

`test_stream_bridge_sender_shared_resource.cpp` compiles the actual production
`submittedSharedResource` guard. The installed luma callback supplies its live
DirectMode handle, generation, process and descriptor, plus the native bound-SRV
resource and its separately opened held resource. Existing device, output and
NVENC COM-object comparisons are unchanged.

Windows Developer Command Prompt:

```bat
cl /std:c++17 /EHsc /W4 /WX /I. tests\test_stream_bridge_sender_shared_resource.cpp pc\stream-bridge-sender-provider-coverage-render.cpp /Fe:shared-resource.exe /link d3d11.lib dxgi.lib dxguid.lib uuid.lib
shared-resource.exe --metadata
shared-resource.exe --gpu
```

The first mode uses explicit simulated COM implementations: different object
identities share an explicit handle. It tests generation/PID/descriptor,
nonshared/foreign-allocation refusals and balanced references. It proves no GPU,
Steam, stream or headset behavior.

The second mode tries **WARP, then hardware** if the first lacks the required
public sharing capability. It creates a genuine shared RGBA8 texture, obtains
its real `GetSharedHandle` value, opens it twice, checks the different IUnknown
identities and actual bound SRV, and exercises the production guard. No GPU
handle value is guessed. The creator wrapper is released while opened wrappers
retain the allocation. Exit **77** identifies an explicitly unsupported sharing
capability, with the device type, failing operation and HRESULT; it is not a
successful GPU result. Genuine contract/guard failures return 1.

Microsoft documents that [opening shared resources creates resource wrappers
whose COM identities can differ](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1),
whereas [GetSharedHandle identifies the shared allocation, including opened
resources](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle).
The held wrapper retains that handle's lifetime. This path deliberately supports
legacy non-NT DXGI handles, with no NT-handle, pointer, dimensions-only or latest
frame fallback. Actual Steam compositing and final headset output remain
separate hardware checks.
