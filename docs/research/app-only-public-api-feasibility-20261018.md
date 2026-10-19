# App-owned routes with an unchanged Steam Link sender

Review date: 2026-10-18. Native source baseline: `5baa8c8`. This is a bounded
public-API review, not an implementation or a new headset result. No Frame,
Steam, game setting, driver or runtime was changed. Only this document is owned
by this research task.

## Decision boundary

The permitted PC component is the game/mod and its own helper/encoder. Do not
inject into, interpose on, replace or patch the PC Steam client, compositor,
vrlink driver or its encoder. A separately installed PC display driver is also
outside this constraint. A Frame receiver may be changed with explicit device
authorization. A mod-owned network/encoding component is additional code, but
is not a modification of Valve's sender.

**No inspected public contract currently establishes a full-quality,
alpha-preserving auxiliary layer through the existing Steam Link video.** This
is an evidence limit, not a claim that Valve's implementation cannot do it.
Creating an app RGBA texture and successfully submitting a local overlay does
not establish its remote carriage.

The current carrier spends real image area on coverage. The `2048/3168` linear
scene fraction remains even if the source render resolution increases. Current
retained hardware observations show MR uniform decode `3264 x 6400`, unchanged
through larger requested widths, while ordinary streaming uses its separate
foveated layout. These are observations of this session, not absolute decoder
or headset limits. More requested width is not a demonstrated correction.

## What the public interfaces actually provide

| Interface | Publicly established capability | Missing contract for this project |
| --- | --- | --- |
| OpenVR application `Submit`, texture bounds/pose, `IVROverlay::SetOverlayTexture`, projection/ordinary overlay transforms | Submit an application's eye/layer texture and request local compositor placement | Independent remote RGBA delivery, actual remote texture dimensions, an encoder generation associated with that submission, and remote composition order |
| OpenXR projection/quad layers and source-alpha blending | Describe local layer images, views, spaces and blending to the selected runtime | A requirement for Steam Link to preserve the layers separately or encode alpha |
| OpenXR environment blend mode | Enumerate and request supported physical-world blending on that runtime/system/view configuration | Permission to select an unsupported PC mode, or a remote alpha transport |
| `ISteamRemotePlay` | Inspect sessions/client information and manage invitations/direct input/cursors | App-owned video/alpha payload injection, decoder-surface access or an exact video-frame join |
| Steam Link external-vendor integration | Resource-only headset/controller integration and documented stream/foveation configuration | App-owned arbitrary payloads, scene alpha plane or encoder-frame callback |
| Valve GameNetworkingSockets | Application-owned network messages | Image compression, access to Steam Link's selected video generation or its warp |

These distinctions follow the [public OpenVR application header][openvr],
[OpenXR projection][projection]/[quad][quad] and [blend-mode][blend] contracts,
[environment-mode enumeration][enumerate], [Remote Play API][remote] and
[Steam Link vendor integration][vendor]. No inspected API above promises that a
locally alpha-blended layer remains separate after a remote video encoder.

The historical [Steam Link SDK][old-sdk] targets the ARMv7 Steam Link set-top
device and its custom firmware. It is not an SDK extension point into the
current Steam Frame VR encoder/decoder. Valve's current [Frame custom-engine
guide][frame] instead recommends OpenXR and Android or Linux ARM64 targets;
it also documents ARM64 Steamworks support starting at SDK 1.63. These are useful
receiver deployment foundations, not a transport-alpha guarantee.

`XR_EXTX_overlay` describes overlay sessions but is explicitly **not ratified**.
Its presence in the registry does not establish support on the installed Frame,
or permit an ordinary second scene session to coexist with Steam Link. An owned
local overlay receiver would need actual runtime extension/session admission
proof before using that route. [Khronos extension reference][overlay-ext]

## Ranked routes under the boundary

Ranking separates implementation control from the size of the change. None is
presented as already working on this device.

### 1. Preserve Steam Link RGB and send app coverage separately: smallest image change, conditional

Keep ordinary RGB/foveation completely unchanged except for an explicitly
designed source-identity carrier, and send coverage using a bounded mod-owned
connection to the Frame receiver. This avoids reducing RGB to the carrier's
central square. It does **not** become correct merely because both network
connections arrive quickly.

Two currently missing obligations are independent:

1. Recover an app-generated session/generation/eye identity from the **selected
   decoded RGB resource**, not from the newest message, a timestamp or a submit
   counter. Any in-band marker must survive the actual composition, codec,
   cropping and moving foveation without becoming a visible quality regression.
2. Map that generation's coverage through precisely the same native
   composition/reprojection/foveation as its color. A source marker does not
   recover a nonlinear warp. An application's pose does not describe unrelated
   dashboard/notification layers subsequently composited on the PC.

This is a conditional branch, not the fastest proven implementation. Reject it
if either contract cannot be established with the unchanged sender. Do not
spend another hardware round merely verifying that a LAN mask can arrive.

[GameNetworkingSockets][gns] can carry reliable control and unreliable frame
messages without depending on Steam or a game publisher's Steamworks privileges.
Its documented features include encryption, fragmentation and reassembly, but
not compression. The receiver still needs its own authenticated pairing,
bounded per-generation storage and deadline/drop policy. Using the existing
game multiplayer connection does not automatically reach a process on the
headset; it is a different endpoint and trust boundary.

At 4040-square stereo and 90 Hz, uncompressed R8 coverage is approximately
2.94 GB/s before transport overhead. Compression must be measured on moving
hands, particles, transparency and changing menus, not only an almost-empty
mask. This arithmetic is not a network benchmark. Missing or late coverage must
leave the correct RGB opaque without stalling the compositor.

### 2. Mod-owned color and coverage together: strongest correctness basis, larger project

Capture the existing rendered stereo contribution and coverage in the mod
without rendering the scene a second time. Encode and transmit both under one
owned immutable generation record. On the Frame, present only complete matching
pairs with the original per-eye pose/FOV and the receiver's current tracking.
This removes the need to identify a color frame inside Valve's independently
scheduled video: the image being presented is the owned color image.

This meets the no-PC-Steam-modification rule, but does **not** retain Steam
Link's video path. A coexisting prototype could initially use Steam Link for
tracking/input and the owned path for presentation. That is a separate
coexistence experiment, not a documented public shortcut: the receiver must
own its projection/reprojection and must not apply a native Steam Link warp
belonging to a different video generation. Simply replacing the sampler in the
existing shader while retaining that unrelated warp is incorrect.

The smallest useful implementation sequence would be:

1. Define `session nonce + generation + eye + extent + view/FOV + color/coverage
   convention + codec/layout revision` and bounded paired-resource lifetimes.
2. Build an app-owned synthetic RGB/coverage generator and owned receiver
   composition, with actual decoder output associated to that record. Start at
   a measured sustainable extent; do not claim that upscaling restores detail.
3. Prove native Frame presentation/coexistence using a public local projection
   layer if actually supported, otherwise an explicitly owned receiver path.
   Ordinary UI quads are not a substitute for stereo scene projection.
4. Connect the mod's actual existing eye output; reuse its images and GPU
   synchronization. Measure latency, decode/GPU load and hard/fractional edges.

Failure to establish coexistence means choosing an owned OpenXR client with
tracking/controller/audio integration, not silently adding a PC driver. Camera
pixels remain local in all variants.

### 3. Public PC overlays: bounded capability branch, not another assumed fix

The existing helper is useful only as a delivery preview. The latest ordinary
mono quad removes the earlier asymmetric-frustum placement error, but local
`overlay_visible=1` and a valid texture extent are still not receiver evidence.
The user did not see it on the Frame. That does not establish independent
transport, nor does it uniquely establish flattening or deliberate omission.

If a later source-proven delivery correction exists, the next evidence would
need to distinguish independent coverage from RGB flattened into the normal
stream, then measure resolution, motion and per-eye projection. A visible
colored rectangle or fractional brightness alone cannot distinguish them.
Do not replace the game source with zero merely because the public API emits
READY. See [the scoped helper contract](../../pc/projection-overlay-probe.md).

## Encoder and reuse choices

The current [NVIDIA Video Codec SDK 13.1 guide][nvenc] documents auxiliary alpha
for **HEVC**, explicitly enabled on a caller-owned encoder after a capability
query. Supported inputs are NV12/ARGB/ABGR; documented exclusions include 10-bit,
4:2:2/4:4:4, subframe mode, weighted prediction and video-memory bitstream output.
NV12 needs a separate alpha input; packed ARGB/ABGR uses its alpha channel. The
guide does not document the same feature as an AV1 alpha mode. This is a
possible own-encoder optimization, not a switch for Valve's current encoder,
an AMD/Intel implementation, or proof the Frame decoder accepts its auxiliary
layer. Baseline prototype design must not depend on that last assumption.

Separately encoded grayscale coverage plus conventional color is more portable
as a design, but requires two actual decode paths, exact generation matching
and measurement of sustained decoder capacity. HEVC-alpha and two-stream
decoding are alternative implementations to validate, not interchangeable
guarantees. A side-by-side RGB/mask image in an **owned** codec pipeline is also
possible when dimensions/bandwidth support it; doing so in unchanged Steam Link
reintroduces the already measured spatial-budget loss.

ALVR provides a useful architectural reference, not a drop-in solution:

- Its [pipeline documentation][alvr-how] explicitly covers tracking, rendering,
  composition, encoding, network, decoding and client reprojection. It describes
  an additional PC SteamVR driver; using that unmodified package does not meet
  this project's application-only PC boundary.
- Its current [video packet definition][alvr-packets] includes timestamp,
  per-eye view parameters and per-frame foveation shifts. That is the kind of
  owned frame contract a replacement needs. It is not a public interface to
  Steam Link's differently owned generations.
- Its current [passthrough setting definition][alvr-settings] contains
  brightness-derived Blend and RGB/HSV Chroma Key. These modes alone are not
  real independent-alpha delivery. A Frame/Linux receiver and actual explicit
  coverage transport would still be new implementation work.

The ALVR architecture article declares itself last updated in 2023. The two
linked current source files were inspected on this review date; do not treat
the old article as current hardware/codec/Frame support documentation.

## Falsifiable acceptance gates before another headset request

- **No sender modification:** inventory shows only mod-owned PC code and owned
  Frame components; no new display driver, injection, proxy or detour.
- **Same-generation images:** fixtures deliberately reorder, duplicate, delay,
  lose and reconnect color/coverage. No wrong-session/eye/generation pair may
  become transparent. A native selected frame is joined explicitly or not used.
- **Actual detail:** record source eye extent, negotiated/decoded extent and
  effective scene ROI. Test moving fine/coarse patterns at a fixed angular size;
  an encoder request/readback or a large allocated texture is insufficient.
- **Real alpha:** opaque black must hide the camera, transparent black must show
  it, and fractional layers must mix them correctly. Test two overlapping 50%
  layers and positive emission at zero coverage. Color classification is absent.
- **Motion and composition:** test both eyes, head movement, changed view/FOV,
  repeated frames, dashboard open/close, loading, resize and reconnect. An
  untracked overlay or unknown warp retains opaque rendering.
- **Performance and escape:** bounded queues and GPU lifetimes, no render-thread
  network wait/readback, measured p50/p95/p99 latency and dropped-pair rate;
  opening options or stopping the owned channel restores the unchanged game.

## Recommended next decision

Do not implement another uniform-width adjustment or treat public layer alpha
as network alpha. If preserving the exact existing Steam Link video is mandatory,
work first on the two association/warp obligations in route1 and stop if no
source-proven solution exists. If a mod-owned video channel is acceptable,
route2 offers the clearest correctness boundary and a meaningful standalone
prototype, with an honestly larger implementation and hardware budget.

This review did not run tests, create compiled artifacts or touch the headset.

[openvr]: https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h
[projection]: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjection.html
[quad]: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerQuad.html
[blend]: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEnvironmentBlendMode.html
[enumerate]: https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrEnumerateEnvironmentBlendModes.html
[remote]: https://partner.steamgames.com/doc/api/ISteamRemotePlay
[vendor]: https://github.com/ValveSoftware/openvr/wiki/Steam-Link-Integration-for-External-Vendors
[old-sdk]: https://github.com/ValveSoftware/steamlink-sdk
[frame]: https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom
[overlay-ext]: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXTX_overlay.html
[gns]: https://github.com/ValveSoftware/GameNetworkingSockets
[nvenc]: https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html#alpha-layer-encoding-support-in-hevc
[alvr-how]: https://github.com/alvr-org/ALVR/blob/master/wiki/How-ALVR-works.md
[alvr-packets]: https://github.com/alvr-org/ALVR/blob/master/alvr/packets/src/lib.rs
[alvr-settings]: https://github.com/alvr-org/ALVR/blob/master/alvr/session/src/settings.rs
