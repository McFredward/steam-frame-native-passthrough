# PC passthrough without hooks in the native Steam sender

> **Historical reference.** Historical review under a superseded constraint. On 2026-10-18 the maintainer authorized the paired PC sender companion. The current route carries independent authenticated coverage beside ordinary video; it does not use the payload atlas proposed below. See [Developing](DEVELOPING.md).

Review: 2026-10-12, against `dev` `89b22e7` and the official SDK sources linked
below. **There is no proved, installable Steam Link alpha route under this
constraint yet.** The preferred investigation is an app-owned explicit-payload
experiment, with an early rejection gate for transport and spatial mapping.
If that gate fails, an owned streaming path is the defensible fallback. More
mirror polling or another unrelated resource capture cannot repair the gap.

The maintainer now prioritizes leaving the native PC sender untouched. The
private sender observer in [PC-NEXT-ADAPTER.md](PC-NEXT-ADAPTER.md) does not meet
that preference, even if it never rewrites a DLL on disk. Injected observers,
vtable detours, D3D/NVENC interposition and proxy DLLs are still hooks in native
processes. They must not silently become the implementation of this plan.

Changes to the existing **GloomhavenVR application rendering code** are a
different boundary: the mod can own its render targets, coverage pass and a
public-API export. An additional public SteamVR driver or a new streaming client
would also be additional software and a different streaming route, even though
it does not patch Valve's sender. “No sender hooks” does not mean “no new PC
software.” This review changes no application, PC pipeline or Frame file.

## Ranked routes

| Priority | Route | Native PC sender involvement | Decision |
| --- | --- | --- | --- |
| 1 | Explicit color/coverage/data regions in the **same existing Steam Link video frame**, authored by GloomhavenVR | No injection or private hook; app output changes | Best bounded experiment for retaining Steam Link. Full-frame survival and matching spatial transforms are unproved. Stop early if either cannot be established. |
| 2 | App-owned color **and** coverage transport to an owned Frame renderer | Native Steam sender is bypassed, not modified | Most controllable fallback: own frame IDs, packing, encoding and reprojection together. Substantially larger client/input/runtime integration; not a drop-in Steam Link feature. |
| 3 | App-owned coverage side channel while retaining Steam Link color | No sender hook only if a genuine in-band source identity and transform contract are proved | Conditional optimization after route 1's association work. A LAN mask by itself is insufficient. |
| Future | Valve-supported alpha/mask extension | Official integration only | Prefer it if Valve supplies a concrete public contract. No such contract is established by the inspected public SDK/docs. Do not schedule implementation around an undocumented capability. |

Valve's published Steam Link vendor integration configures resource-only
drivers, headset/controller resources and foveation-related settings. It does
not document an application mask plane, arbitrary video metadata export or
encoder-frame callback. This is a limit of the inspected public contract, not
proof that Valve could never add one. [Steam Link vendor documentation](https://github.com/ValveSoftware/openvr/wiki/Steam-Link-Integration-for-External-Vendors)

## What app-side alpha can establish

GloomhavenVR first needs genuine per-eye coverage from its own immutable color
generation: transparent clear, antialiased opaque edges, partial transparency,
and declared straight/premultiplied RGB. Its current PC chroma output is opaque;
see [PC-MASK-SOURCE.md](PC-MASK-SOURCE.md). This source change is useful in every
route. It does not itself make Steam Link carry alpha.

OpenVR `Submit` accepts an eye, texture, bounds and optional pose/depth flags;
it returns compositor status, not a Steam Link encoder identity. The D3D11
mirror API returns an undistorted composited eye SRV, with a release API, but
no frame ID or guaranteed scene-alpha contract. Frame timing is compositor
history, not an atomic relation between an acquired mirror and encoder input.
[Official OpenVR application header](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr.h)

The previously inspected native encoder input is NV12/P010. Those formats have
luma/chroma planes, not a scene-alpha plane. An RGBA application texture cannot
recover alpha after that conversion. [DXGI format definitions](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)

NVENC offers explicit HEVC auxiliary-alpha encoding, with a capability query,
encoder configuration and alpha input. Its documented restrictions include
10-bit input. This could serve an **owned encoder** after checking device and
Frame decoder support; it is not an app-level switch for Valve's existing
encoder or evidence that Steam Link enables or transports that auxiliary layer.
[NVIDIA encoder SDK](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html#alpha-layer-encoding-support-in-hevc)

## Same-frame packing: precise opportunity and missing proof

The app could construct, per eye, a canvas containing its color, explicit
grayscale coverage and a robust data header. Coverage comes from the renderer's
alpha; decoding designated payload pixels is **not** classifying scene RGB by
a key color. An app-owned session nonce, immutable content ID, eye and layout
revision can be encoded in that header. Such an ID is useful only because it
is physically attached to the matching owned color/coverage generation and
recovered from the selected decoded surface. It is never assumed equal to a
submit count, presentation index or native TX key.

This has one advantage: when the complete payload survives in one selected
decoded frame, mask arrival cannot race a separate color packet. Native frame
repetition can repeat the entire payload. It still has four unresolved gates:

1. **Carriage.** Payload outside the app's submitted bounds is excluded rather
   than becoming a hidden auxiliary plane. Payload inside the sampled canvas
   occupies real image area. Public `Submit` does not promise that a larger
   app texture enlarges Steam Link's encoded frame. Reserved guard bands must
   demonstrably survive crop, scaling, compositor clipping and codec processing.
   [OpenVR bounds/submit contract](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr.h)
2. **Matching rays.** Color and coverage stored in different canvas regions are
   transformed at different rays by compositor reprojection and streaming
   foveation. Equal-frame membership does not make them spatially registered.
   For logical pixel `p`, packed placements `c(p)` and `a(p)` require the actual
   native mapping `W` at both `W(c(p))` and `W(a(p))`. A constant tile offset
   after a nonlinear `W` is insufficient. The receiver must establish that
   mapping and its revision, then apply its own later eye reprojection equally
   to color and coverage. A static calibration is not proof for moving gaze.
3. **Composition.** The app's alpha describes its layer, not subsequently added
   SteamVR dashboard, notification, loading or other-application layers. A
   visible data marker alone does not prove those layers are absent elsewhere.
   Unknown composition must remain opaque. Motion smoothing can also synthesize
   content rather than repeat an intact app generation. The public
   `Submit_FrameDiscontinuity` flag prevents extrapolation from a frame pair;
   it does not promise to disable all reprojection, composition or foveation.
   [OpenVR submit flags](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr.h)
4. **Numeric fidelity.** Codec quantization, filtering, video range and transfer
   functions can alter an explicit grayscale value and header bits. Establish
   the complete conversion, validate header corruption/reconnect rejection and
   measure fractional-alpha/edge errors. Payload loss means opaque output, not
   selecting a nearby or newest mask. Luma/chroma knowledge alone does not prove
   the active conversion or spatial mapping.

Do not use the other eye as the mask plane: it removes that eye's color. Do not
alternate color and mask across video frames: that reintroduces loss/pairing and
latency. Hiding extra coverage in RGB low bits has no established survivability
through this lossy pipeline. None of these shortcuts preserves the required
stereo presentation.

## Side channel and controlled transport

A separate authenticated R8 channel can reuse [MASK-TRANSPORT.md](MASK-TRANSPORT.md)
and avoid video compression of coverage. It still needs a real relation between
its source generation and the **selected native color**, plus that color's
composition/reprojection/foveation mapping. A proved in-band identity could
provide the first relation; it cannot provide the second by itself. Public
mirror acquisition, timestamps, newest-mask selection and compositor counters
are not substitutes. Late/missing coverage retains opaque color without
blocking the compositor.

For an owned route, capture color and coverage in the app's rendering path
before native streaming composition, encode them under one owned frame/eye
record, and decode/present them together on the Frame. The owned renderer must
also own or explicitly integrate tracking, controller input, prediction,
reprojection, connection resets and GPU lifetimes. Keeping Steam Link solely
for tracking while independently replacing video is a separate coexistence
problem, not an established public shortcut. Start with a Gloomhaven-specific
client; generic SteamVR application support is a larger project.

A new public display driver can expose `IVRVirtualDisplay::Present` and its
`nFrameId`, but that ID belongs to its own display path. Registering it is not a
tap into the installed vrlink driver, which the earlier source audit found uses
DirectMode 009 rather than VirtualDisplay 002.
[Official driver header](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_driver.h),
[existing native-path evidence](PC-DRIVER-20261011.md)

The Frame camera stays local in every route. An owned OpenXR receiver can
request alpha blending only when its active runtime enumerates that blend mode
for its system/view configuration; its layer alpha convention must also match.
Standalone success does not establish a PC Steam Link transport capability.
[OpenXR environment/layer blending specification](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc)

## Required evidence and measurable compromises

For each accepted display use, retain the source session/content generation,
eye/slice, color/coverage extents and bounds, render pose/projection, actual
selected video/surface content generation, layout/conversion revision,
composition and reprojection/foveation mapping, and acquired GPU resource
lifetime. In a same-frame route those source facts must be recoverable from
that selected resource's payload; a separate-channel route needs an explicit
joined record. New sessions, eye swaps, extent changes, missed lifecycle events,
reused resources and ambiguous synthetic frames invalidate the association.
Unknown new pixels/layers remain opaque.

Compare proposed modes with the unchanged native stream, using measured p50,
p95 and p99 display latency, packet loss, opaque-fallback frequency, GPU work,
decode work and color/coverage edge error. Useful quantitative limits are:

- At fixed encoded pixel area, allocating half to full-resolution coverage
  leaves half for color. Packing across one axis halves that axis's color
  samples; an ideal uniform redistribution scales both axes by about `0.707`.
  This is area arithmetic, not a supported Steam Link layout/resolution promise.
  Lower-resolution coverage saves area but sacrifices thin edges/partial alpha.
- Two uncompressed `2048 × 2048` R8 eyes at 90 Hz require about **755 MB/s**
  (**6.04 Gbit/s**) before headers. Compression/deltas must be benchmarked;
  highly changing particles and transparency can defeat sparse-mask assumptions.
- One extra whole-frame wait costs **11.1 ms at 90 Hz**. Do not hide side-channel
  delay by accumulating frames. Spatial packing avoids that pairing wait only
  after its carriage/mapping gates pass; it can still add GPU/decode work.

## Preferred next step and stop conditions

Prepare an app-owned source test scene and payload specification first. Test
opaque green/red, transparent clear, 25/50/75-percent coverage, overlapping
translucency, MSAA edges and asymmetric left/right coordinate grids. No sender
hook or mask activation is needed to design these controls.

The [offline app-owned payload probe](APP-PAYLOAD-PROBE.md) now supplies a bounded
RGB/R8 canvas, redundant source headers, asymmetric fixtures and strict reference
rejection controls. It does not yet attach to Gloomhaven's render output or prove
moving material/MSAA coverage, native carriage or spatial mapping. Implementing
that app-output adapter is the next transport prerequisite; repeating the prior
YUV resource census alone does not advance it.

Then obtain separately scoped approval for a reversible **app-output/Frame-side**
feasibility test, not a PC sender observer. Require both payload planes and
their IDs from one selected decoded resource; prove registration during head
movement and moving gaze, resolution/foveation changes, repeats/drops,
dashboard/focus changes and reconnects. Test only modes whose full transform and
composition contract can be established; an apparently aligned static picture
does not pass. Require **zero accepted wrong-session, wrong-eye or wrong-source
associations** in those controls; intentionally damaged, clipped and ambiguous
headers must produce opaque fallback. A transform/layout revision change cannot
reuse an earlier calibration. Record edge displacement in decoded luma pixels
and alpha error against the source reference; agree acceptable quality/latency
limits before interpreting a lossy mode as a success. Missing payload, untracked
layers, unexplained UV error or
unacceptable quality/latency is a **stop**, not a reason to substitute timing
guesses or quietly add a native sender hook.

If full-plane carriage fails but an explicit source marker and complete mapping
both pass, assess the side channel. If either association or mapping remains
unavailable through the untouched sender, stop that Steam Link branch and
choose an owned route or seek an official Valve extension. Reuse the current
Frame resource/lifetime work where relevant; it does not supply this missing
PC composition contract by itself.

This document-only review checked official sources and existing local evidence.
No implementation, private pipeline, Frame service or main Gloomhaven checkout
was changed. No runtime, throughput or headset result is claimed.
