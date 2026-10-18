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
listener only when exactly one actual initialized HEVC encoder has accepted an exact registered/map/Encode submission. Init alone cannot open the network listener. It wires
the real sender sealer/receiver TCP replies into that observed encoder, resets
on source generation loss, and originates a fresh coded-AU probe after mutual
confirmation. No timer expires a feature or restarts SteamVR.

The worker executes on the existing provider thread; native frame/encoder
callbacks do no socket/file work. At most 32 state transitions are logged.
Unsupported codecs, missing/mismatched peers and actual EOF leave ordinary
VCL bytes and image dimensions unchanged. A transport/handshake/probe is not
coverage permission. The new native GPU observer registers the certificate/mask `OutputAugmenter`; its independent source, two-eye and immutable-readback guards must pass before a mask can be attached.

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
The GPU observer described below checks actual acquired source/SRV identities and native luma output; DirectMode metadata alone still does not prove any of them.

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

## Genuine native GPU coverage and app source lease

The public successful NVENC Open supplies its actual D3D11 device. A cold worker
refuses `D3D11_CREATE_DEVICE_SINGLETHREADED`; otherwise it obtains the immediate
context and explicitly enables its public `ID3D11Multithread` protection before
admitting companion work. That protection remains enabled for this process
lifetime; a normal fresh SteamVR process restores the original policy. Its
serialization can have a CPU cost and must be measured in the integrated run.

The already hash-verified module pins four **public D3D11 call sites**:
Map return `9f36f2`, Unmap `9f372c`, luma Draw `9f3865`, and NVENC input
CopyResource `a217fb`. No private function is invoked and no native shader source
is distributed or patched. The original typed methods are always forwarded.
Other contexts/call sites remain ordinary D3D11. Cleanup compare-exchanges only
our own published vtable entries; callable objects stay alive until process exit.

Cold imports open only the real DirectMode swapset handles with their exact
owner, lifetime generation, description and canonical COM identities. At the
actual Draw, the native CB2 Map/Unmap receipt supplies the current eye, while the
bound SRV0 must match that eye's actual acquired shared resource. Every layer
and both eyes must be accounted for inside the same current DirectFrame and
registry epoch. Unknown resources, duplicate/missing eyes, incompatible native
output extents/formats, contention or retired ownership cannot produce a mask.

The actual original luma PS already outputs source alpha. A preallocated R8
companion stores transmittance, cleared to1 for each real present serial; its
blend equation is `T = T * (1 - nativeSourceAlpha)`. It reuses the bound native
PS/VS/CB/SRV/sampler, geometry, scissor and viewport, preserving the original
foveation mapping. It never classifies RGB or changes the encoded image size.
The native target and blend state are restored; an attached depth target is
refused. The CPU result is `A = 255 - T`.

A separate tiny5x5 grid per submitted layer/eye is copied from strictly interior
pixel centres of the **actual source** RGBA/BGRA8 texture. The old v3 atlas always
writes alpha1; at least one interior texel below1 **for every submitted eye/layer** is required to admit ordinary
RGBA. Encoded-frame padding or samples outside submitted bounds do not establish
that transition. A genuinely fully opaque source remains correctly opaque until
such a witness exists. Typeless members of the same8-bit format family may be
copied; unsupported formats/mips/arrays/MSAA refuse the route.

The exact native CopyResource records an immutable R8+source-witness readback
in one of four bounded slots for its actual destination resource. Register →
Map → accepted Encode freezes that slot under the encoder generation and unique
submission identity. Lock may consume only the uniquely matching output/timestamp
receipt. `DO_NOT_WAIT` refuses incomplete GPU readback; it never substitutes a
later image, waits for GPU completion or evicts another frozen slot. Compression
and authentication occur outside the GPU metadata mutex. Optional-mask failures
can still attach a byte-only certificate; that certificate never permits MR.
The actual peer decoder byte-capacity challenge limits the complete final AU
transactionally. Excessive/incompressible data restores the exact original AU.

A private per-user, local-only named pipe exposes the fixed128-byte `SBN1`
source-preparation contract. Its kernel client PID must match the request PID;
that PID must own the actual source swapsets and have a fully observed two-eye
GPU route. The reply echoes the request nonce and active paired binding/encoder
generation. Flag1 additionally requires mutual compatibility and an exact
coded-AU roundtrip. Flag2 is **not emitted**: a local reply cannot prove remote
selected-image rendering. Opening/closing the live app pipe does not invent
frame identities. EOF, worker failure and provider Cleanup revoke it without a
feature-expiry timer. Socket/file work stays on the cold worker.

## Verification and remaining runtime observations

Focused checks: input/device/output43, DirectMode20, provider/control/SEI/TCP
composition47 pass normal and ASan/UBSan; source IPC has167 hand-checked framing
and refusal assertions. Earlier unchanged original-boundary54 and sender
capability81 checks are inherited. Actual host D3D11 execution through Wine's
software GPU passes21,646 assertions for native-PS-alpha R8 blending, original
RGB/target/shader preservation, native eye viewports, layered coverage, source
interior bounds and immutable fractional-alpha witnesses. Two causal GPU
mutations (RGB substituted for alpha, and ignored submitted ROI) are rejected.

This environment refuses creation of an NV12 render target. Therefore the
entire live native NV12 frontend is **not** represented as a completed GPU test:
its exact association/lifetime predicates have public-call fixtures and static
source pins, while its R8 and source-witness operations have real D3D11 tests.
Actual Windows driver load order, device/context equality, shared-resource
imports, current native composition/encoder call chain and the headset picture
remain integrated runtime observations. The Frame selected-surface renderer is
a separate parent-owned integration. No hardware success or installability is
asserted by this host checkpoint.
