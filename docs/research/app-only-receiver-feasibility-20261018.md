# App-only source and Frame receiver: sharpness feasibility

Research-only review of native `dev` `5baa8c822ca0e7e2ab47dce17b9a8ffa7e5ddb0d`.
No code, settings, devices, shader or transport was changed; no tests/builds were
run. Only the game/mod and Frame receiver may change. Injection, detours, proxy
DLLs, encoder hooks and modified Steam PC binaries are excluded.

## Decision

**Investigate A with one bounded local registration prototype; keep C as the
controlled fallback. Do not repeat an overlay or target-width experiment.**
A can preserve the ordinary Steam Link RGB/foveation path, but is conditional on
proving the selected image's source identity **and** its mapping to application
rays. B can reduce today's atlas penalty, but cannot promise unchanged quality.
C owns the complete association and is the cleanest architecture if the untouched
PC compositor prevents A's mapping/composition contract from being established.
It is also substantially more work than a receiver shader replacement.

The working v3 alpha/edge result is not a full-quality result. Its scene rectangle
is 2048-square inside 3168-square; actual decoded allocation did not follow the
larger requested widths. Total allocation is not per-eye active detail, especially
when comparing uniform and native foveated formats. See the retained
[quality review](stream-quality-comparison-20261018.md) and
[v3 layout](../protocols/scene-contribution-atlas-v3.json).
Build 707's local `overlay_visible=1` and accepted 2048-square quad did not establish
visible remote delivery. Public overlays are therefore not this plan's shortcut.

| Route | What changes | Quality opportunity | Unproved boundary |
| --- | --- | --- | --- |
| A: normal RGB + app mask side channel | Small app marker, separate mask transport, new Frame binding/sampler | Keep normal RGB extent and foveation | Exact selected source, PC prewarp/composition, filtering and GPU mask lifetime |
| B: compact in-band coverage | New source layout/codec and receiver decoder | Give much more of the existing image back to RGB | Worst-case mask capacity, symbol survival and the same spatial mapping problem |
| C: owned RGB+mask stream | Encoder inside mod; owned Frame renderer/client | No v3 area loss; own source/pose/coverage chain | Codec/latency, Frame composition, tracking/input/session integration |

## A: direct source identity is possible in principle, not implemented

Create one immutable app record containing session nonce, full 64-bit content
pair ID, eye, actual color/coverage extents, valid rectangle, transfer convention,
render pose/FOV, layout and mask-content digest. Color contribution `E` and
coverage `A` must come from that same rendering generation. A paired mask is
linear coverage, not color classification. Preserve `E+(1-A)*camera`; positive
additive `E` at `A=0` is valid content, not an empty pixel.

A robust in-band token could occupy an application-owned region proven to have
**both `E.rgb=0` and `A=0`** for the entire declared replacement footprint. This
can avoid sacrificing *visible* RGB detail or a large fixed RGB rectangle. The
receiver would restore only that explicit metadata footprint to zero. Merely
finding alpha 0 is insufficient: overwriting additive light would destroy content.
If no sufficiently large zero-contribution region exists, do not insert a token.
Peripheral foveation/codec filtering can destroy small symbols; no cell size,
watermark robustness or spare-space guarantee is established by this review.

Decode the token from the **actually selected decoded eye image**, and join it
to the mask by full session/content/eye/extent/layout. Repeated presentation uses
the matching retained GPU mask lease. Never choose the newest mask, an approximate
timestamp, a Vulkan allocation generation, or a submit counter. Header CRC is
corruption detection, not authentication or proof of the entire composed body.
Bind networking to an authenticated session and reject contradictory identities.

Do **not** force this into `FrameAssociationLedger::recordNative` by inventing a
Steam transmitter key. That method explicitly requires a proved composition /
encoder /16-bit TX-key chain which this app-only route does not supply. A new
**direct app-token-to-selected-image association** should record source evidence,
selected image/view/content generation, native sampling cohort and GPU mask lease.
It must track content reuse in the same allocation, reset/extent changes and
stereo-pair readiness; ambiguous/late/missing evidence retains opaque output.
The existing bounded ledger and mask cache offer useful policies, not an adapter.

### The harder part: identity does not supply the missing PC mapping

Let `p` be an app-eye sample and `W_n(p)` its coordinate after the PC compositor's
actual warp/composition for selected video generation `n`. The Frame's native
vertex and fragment provide a current reprojected source coordinate `q`. To use
an app R8 image at a displayed color sample, the receiver needs the corresponding
`p = W_n^-1(q)`, including valid domains and filtering footprint. The same later
Frame reprojection must affect both color and coverage.

The existing `%883` cancellation normalizes the current native `R1` rectangle;
it is valuable and source-derived. It is **not proof that `q` equals an app-submit
UV before PC composition**. The native color path also reconstructs inner/outer
samples with distinct weights. A high-resolution mask sampled at an unrelated
or differently filtered coordinate can recreate edge halos despite a correct ID.
See [the exact q/R1 scope](../FRAME-UV-BINDING-20261011.md) and
[retained registration limits](native-r1-registration-20261017.md).

A pure rotational known-pose model can use a projective mapping derived from
source/target pose and FOV. That is a useful test model, not a declaration that
all Steam Link frames obey it. Translation with depth, disocclusion, synthesized
frames, dynamic foveation and additional PC layers require their own proven
mapping/composition information or rejection. A few fitted fiducials do not prove
unobserved pixels obey a homography. An intact corner token can coexist with a
notification or mixed-generation body elsewhere. Unknown contributions must not
be assigned app-only alpha. Public OpenVR Submit returns status, not this complete
post-composition encoder contract.

The current receiver is a cold pipeline-creation replacement. It adds no per-frame
mask image, descriptor, selected-frame decoder or queue synchronization. A needs
new bounded reception/upload workers, admitted descriptor/layout ownership,
GPU visibility/fences, and current-image joins before consumption. Neither the
current same-image v3 format guard nor its 32 status updates provides that binding.

## B: smaller metadata/coverage regions improve the budget, not for free

A lossless mask tile format can encode constant 0/255 blocks, mixed blocks and
bounded deltas, with exact source identity and integrity checks. Reserve a small
explicit payload area and reject overflow instead of truncating the mask. Retain
all fractional coverage and additive RGB; do not threshold glass or distinguish
windows from other materials. Compressing those bytes for a designated video
region is not chroma-key replacement.

No recorded game-mask complexity distribution proves the required compression
ratio. Noisy particles, MSAA boundaries and translucent overlap may exceed the
reserved capacity. A lossy video path also needs robust symbol coding, checks and
measured decode error; original byte hashes cannot validate changed codec pixels
by themselves. B shares A's PC-warp/composition problem, and an expanded RGB ROI
alone does not restore the native foveated inner density. It is a measurable
compromise candidate, not a full-quality guarantee.

The existing MRA1 encoder hard-limits 4096 per axis and 16 MiB per decoded mask;
the receiver defaults match those limits. Actual 5050/6060 app targets exceed that foundation. Negotiate new explicit
extent/memory limits or refuse the mode; never silently shrink masks or cap RGB.
Raw 4040-square stereo R8 at 90 Hz is about 2.94 GB/s before headers. Existing RLE and
packet helpers are not evidence of a sustainable 90 Hz production stream.

## C: own the stream instead of reconstructing an unknown composition

The mod may own a native encoding component which reads its immutable stereo
`E/A` generation, sends compressed RGB plus exact/compressed coverage with a
common source/eye/pose record, and keeps all work outside the render thread.
This modifies the game, not Steam's sender. No special alpha codec is assumed:
a normal supported RGB codec plus an owned mask plane/envelope is sufficient
architecturally; actual codec, bandwidth and decode cost still need measurement.

An owned Frame renderer can join both planes before presentation and submit a
single matched projection generation. It must enumerate an available environment
blend mode and use compatible layer-alpha conventions. OpenXR defines how alpha
composition works; it does not promise that an arbitrary concurrent Stream Link
session accepts a second renderer or exports its tracking association.
[OpenXR rendering contract](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#compositing).

The defensible baseline is an owned client/connection with tracking/input,
clock and reference-space calibration, prediction, stereo swaps, controller/haptic
return, decoder/image fences, focus/menu transitions and reconnect handled
explicitly. The camera stays local. Keeping Steam Link only for input while
replacing its video is a separate hybrid design: it still needs source-pose /
tracking-space association, concurrent composition and duplicate-work control.
It is not a free shortcut. Native full-resolution color/alpha does not itself
solve latency, refraction or Frame decoder performance.

## Smallest useful next prototype: local evidence before hardware

Build an **owned source/selected-image contract fixture**, without touching the
PC sender or Frame installation:

1. Export real app `E/A` fixtures with render pose/FOV and immutable IDs. Include
   true zero-contribution space, alpha 0 additive light, MSAA edges, overlapping
   translucency and moving asymmetric eye grids. Insert the app token only into
   validated `E=0,A=0` space; verify untouched visible RGB byte-for-byte before
   codec conversion. Unknown source extent or missing free space must refuse.
2. Run a real local supported RGB codec round trip and the existing mask transport
   helpers under drops/reordering/reuse. Associate each selected image with its
   decoded token and a separate exact mask lease. Include stale-eye/session/body,
   extent changes, reconnect, one-eye loss and repeated video presentations. A
   claimed wrong-source acceptance count must be zero; CRC-only token success
   must never imply whole-body/geometry authority.
3. Drive the actual receiver sampling/composition in a local Vulkan fixture using
   explicit known identity, projective and declared nonlinear mappings. Compare
   against an independent source-rendered oracle for color detail, coverage and
   edge displacement. Wrong/unknown PC warp, composition or source generation
   must retain ordinary opaque rendering, not a nearby/latest mask. Reuse the
   current original-R1 reconstruction evidence without extending its authority.
4. Report separately: token recovery, exact mask/source joins, color sample budget,
   alpha/edge errors, compression overflow and CPU/GPU work. Keep actual Steam Link
   geometry/composition authority **false**: an owned codec/simulator cannot prove
   the untouched proprietary sender's behavior.

This prototype is worthwhile for A and reusable for C. Before an A hardware trial,
require a concrete source-derived explanation of `W_n` and selected-frame/body
composition through the actual untouched sender. If that cannot be established,
stop A/B rather than repeat increasingly broad headset diagnostics. C then has
clearer ownership, with a larger but explicit engineering scope.

Public references: [Valve application SDK](https://github.com/ValveSoftware/openvr/blob/v2.15.6/headers/openvr.h),
[Steam Link vendor contract](https://github.com/ValveSoftware/openvr/wiki/Steam-Link-Integration-for-External-Vendors),
[OpenXR frame/rendering specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#frame-loop).
The inspected public contracts do not expose an app mask plane or exact Steam
Link encoder callback; that is an evidence boundary, not a universal impossibility
claim. Source foundations: [mask protocol](../../mask/mask_protocol.h),
[association ledger](../../mask/frame_association.h),
[receiver cold path](../../layer/native_presenter_receiver.h), and
[scene output generator](../../tools/native_fragment_scene_presenter.py).
