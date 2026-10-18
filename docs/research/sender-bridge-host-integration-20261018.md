# Version-pinned PC sender bridge: bounded prototype

This prepares a real Windows public-API interception boundary. It is **not** an
installed bridge, an enabled coverage transport, or a successful Steam Link test.
No PC/Frame runtime, library file or Steam registration was changed.

## Concrete insertion route

The maintainer's current Windows `driver_vrlink.dll` SHA-256 matches the retained
13,908,632-byte module:
`1e846be041039798580a91452371f6288c614fb4c152126b137e2c693fb77eef`.
The private static examination shows:

| Boundary | Exact module RVA / result |
| --- | --- |
| Encoder load | `a1e59c` passes `nvEncodeAPI64.dll` to the loader; `c224bc` calls `LoadLibraryExW` with flags 8. |
| Export lookup | Public `GetProcAddress` import slot `c2c388`, reached through a tail helper at `9db060`; factory lookup call `a1e602`. |
| Factory ABI | `a1e660` requests function-list version `7102000b`; caller-owned table has 2552 bytes; public factory call `a1e69b`. |
| Session creation | `a1e6f1` calls the public table's `OpenEncodeSessionEx`. |
| Output lease | `a1df62` calls public `LockBitstream` (table offset `90`). |
| Copy before release | Returned size is used for allocation at `a1e112`–`a1e121`; returned bytes are copied at `a1e137`–`a1e142`; only then does `a1e156` call public `UnlockBitstream` (offset `98`). |
| Transmission | The worker passes its **copied** bytes and slot metadata to `a2e590` at `a1dd61`, after that lock/copy/unlock helper returned. |

Addresses identify static evidence, not private functions our code invokes.
No Valve binary, shader or disassembly body is redistributed.

The Windows prototype implements an ordinary
[`IServerTrackedDeviceProvider_004`](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_driver.h).
Its manifest requests `alwaysActivate`; it provides no HMD/device. A cold worker
waits for the loaded `driver_vrlink.dll`, verifies the exact file and its loaded
executable-section hash (no relocations in that section), and compare-exchanges
**only that module's** `GetProcAddress` import slot. This is not a process-global
API detour and does not patch a Steam executable or private function prologue.
All other symbol/ordinal requests are forwarded once with their original result
and last-error state. Only a future lookup of the exact NVENC factory from the
actual `nvEncodeAPI64.dll` handle receives our wrapper.

The wrapper invokes NVIDIA's original factory once. It changes the checked public
function-table entries only for the exact reviewed SDK 11.1 table ABI and a
consistent original function set. Session creation, HEVC initialization,
reconfiguration and destruction establish bounded source lifetimes. Successful
Lock results can substitute an owned complete Annex-B AU; only pointer and byte
count change. Original status, timestamps, picture data and input handles remain
unchanged. Bytes stay owned until the matching successful Unlock or successful
encoder destruction. Reconfiguration/Cleanup revoke permission without freeing
an outstanding borrowed AU. Unsupported versions/codecs, errors, absent authority,
subframe polling, capacity refusal and overlapping locks retain the original output.

This matches NVIDIA's documented
[function-list and bitstream lock/unlock API](https://docs.nvidia.com/video-technologies/video-codec-sdk/11.1/pdf/NVENC_VideoEncoder_API_ProgGuide.pdf).
It avoids dependence on a private encoder object's address or ABI. Its bounded
fake-public-table tests execute these callbacks, including deferred Unlock and
cleanup, rather than merely simulating a proposed private jump.

## Explicit current runtime (2026-10-18 follow-up)

The version-pinned provider exports both Server004 and the documented CompositorPlugin001 (loaded in vrcompositor.exe), with no replacement device/component. Each process owns only its observed module/encoder lifecycle. The version-pinned provider now has an actual cold owner for `ControlRuntime`.
Public `IVRSettings_003`, section `driver_steam_link_native_passthrough`, must
explicitly enable `enableStreamBridge` and provide numeric private/loopback
`controlListenAddress` and `controlPort` (1–65535). Missing/default-off settings
retain ordinary video. The selected private pairing comes from the installer's
`%LOCALAPPDATA%/steam-link-native-passthrough/pairings` directory via the existing
strict loader. Keys never appear in settings/logs/argv. The worker opens the
listener only when exactly one actual initialized HEVC encoder exists. It wires
the real sender sealer/receiver TCP replies into that observed encoder, resets
on source generation loss, and originates a fresh coded-AU probe after mutual
confirmation. No timer expires a feature or restarts SteamVR.

The worker executes on the existing provider thread; native frame/encoder
callbacks do no socket/file work. At most 32 state transitions are logged.
Unsupported codecs, missing/mismatched peers and actual EOF leave ordinary
VCL bytes and image dimensions unchanged. A transport/handshake/probe is not
coverage permission. The new certificate/mask `OutputAugmenter` port still needs
the actual GPU route to register it before any MR readiness can be granted.

Five additional **public** NVENC callbacks track RegisterResource → Map →
EncodePicture → LockBitstream. Records are bounded (four encoders; 32 resources,
maps and submissions per encoder). Resource/map generations are separate from
submission ordinals and encoder epochs. A successful Encode (or documented
NEED_MORE_INPUT queueing) and a unique exact output-handle/output-timestamp pair
are required. Input format/extent must agree with registration and initialized
coded extent. An arbitrary latest input, timestamp approximation, encoder slot
or application counter cannot supply this evidence. Ambiguity/failure/retired
resources stay unassociated. An input observer can freeze a genuine immutable
R8 copy receipt before the original Encode under its native submission key.
The later output augmenter receives exactly that accepted key. Final bytes are
owned independently through matching successful Unlock, including when no
handshake SEI was needed, and survive reconfigure/failed Unlock.

With explicit enablement the checked DirectMode009 eight-entry vtable is also
compare-exchanged transactionally after both the native file and loaded code
pins pass. Every original call is forwarded exactly once. Actual
CreateSwapTextureSet PID/description/three handles, destruction and submitted
per-eye bounds/projection/pose establish ownership, not GPU completion. A
bounded `DirectFrame` is available **only** inside the real original Present or
PostPresent call and only while its registry epoch remains valid. Unrecognized
handles, invalid geometry, overflow and lifetime conflict return no source.
The native D3D11 draw must still establish actual acquisition, source sample,
foveation/UV and matching R8 output; metadata alone does not prove any of them.

The provider must be loaded before a future NVENC factory and before the app's
swapsets are created. Unknown preexisting swapsets are never reconstructed from
pointers. A different driver hash or changed original vtable stays inert. No
real Windows/Steam/Frame execution is established by a cross-build or the local
public-call/loopback fixtures. No image route/installability is enabled by this
checkpoint.

## Lifetime and removal

Before publishing callbacks, the provider pins its DLL for the process lifetime.
It also retains source/NVENC modules: cached function tables can outlive provider
Cleanup. Cleanup stops the worker, revokes future augmentation and restores its
IAT entry using compare-exchange only if the entry is still ours. It does not
replace another hook's later change, unload retained callbacks, restart SteamVR,
or free an outstanding locked AU. A process restart is the clean unload boundary;
there is no automatic expiry/restart and no runtime uninstaller in this prototype.

The separately gated setup package
registers its own driver directory when eligible and remove that registration to uninstall;
it must not replace Valve/NVIDIA files. Startup order and whether another hook is
present remain hardware observations, not claims from the local build.

## Offline build and provenance

Run `pc/stream-bridge-sender-provider-build.sh SDK_DIRECTORY PROTOCOL_ROOT OUTPUT`.
Inputs are explicit; the build performs no network download or activation.

| Private SDK input | Source / SHA-256 |
| --- | --- |
| `nvEncodeAPI.h` | NVIDIA-authored MIT header, [nv-codec-headers tag n11.1.5.2](https://github.com/FFmpeg/nv-codec-headers/blob/n11.1.5.2/include/ffnvcodec/nvEncodeAPI.h); `be0c29a41ae3260fd016a9001fd959e747bac93ad21411759736abba4ffdea86` |
| `openvr_driver.h` | Valve OpenVR BSD-3-Clause repository, [commit 0924064316de3effbcd1acf1e309182a2deb1c05](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_driver.h); `1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab` |

Headers are not vendored here. Their original licenses remain with the private
SDK input; the build pins exact bytes. The compiled DLL imports only Windows
system libraries; no NVIDIA/Valve implementation library is redistributed.

The coded-AU adapter is portable C++17; its host tests also run on Linux. The real
loader is currently a Windows x64 implementation. No Linux ELF loader/provider
or AMD/Intel backend is implemented, and a Windows cross-build is not a Windows,
Proton, NVIDIA or headset execution. The same game running under Proton does not
make the native SteamVR driver use the Windows NVENC loader.

## Remaining coverage work

This hook can carry tiny metadata before FEC without resizing/re-encoding RGB.
A full native-coverage bridge still needs the genuine pre-YUV coverage source,
precise source/encoder/transmit association, bounded mask encoding, receiving
backend, and verified current-frame/eye/UV use. The existing
[driver dataflow report](../PC-DRIVER-20261011.md) explains those separate identity
domains. Neither a working SEI append nor public API interception supplies them.

Focused host checkpoint: original public-boundary 54, exact input/device/output association 41, DirectMode ownership 18, actual provider/control/SEI/TCP/probe composition 46, and sender capability policy 81 checks pass. All affected fixtures pass ASan/UBSan. Four semantic controls independently reject wrong output timestamps, failed Encode acceptance, stale DirectMode registry epochs and retained transport trust after EOF. The final Windows DLL links against system libraries only. The CompositorPlugin interface, actual Steam load ordering and headset picture still require the integrated runtime test; no hardware outcome is claimed.
