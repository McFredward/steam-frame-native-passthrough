# Developing Steam Link Native Passthrough

This is the current implementation guide. The [installation guide](INSTALL.md) covers
setup and the [compatibility page](COMPATIBILITY.md) separates the visible
headset result from software-only checks. Dated research pages describe their
own historical checkpoints; a statement there that a later feature is missing
does not override the current implementation.

## Two current routes

The **stable Frame-only atlas route** below has a visible headset result but
uses part of the video image for opacity, with a known sharpness tradeoff.
The **paired Windows/NVIDIA/HEVC preview** uses a separate sender/receiver
companion and exact source/frame/eye association; it is intended to preserve
ordinary RGB quality but has not yet passed a combined headset picture test.
Use [Compatibility](COMPATIBILITY.md) for current evidence and
[PC + Frame setup](PC-FRAME-SETUP.md) for the installed preview workflow. Dated
research pages retain historical checkpoints, not current user instructions.

## Paired preview architecture

```text
Game: current eye E + A
  → admitted ordinary-RGBA source
  → Windows/NVIDIA sender companion: exact source/eye association
  → authenticated coverage in the same HEVC access unit as colour
  → matching Frame import/decode/eye association
  → guarded native composition over the Frame camera
```

The preview must establish compatible peers and actual source/frame/eye/extent
association before activating coverage. A format capability grant is separate
from operational coverage; a handshake alone is not proof of the final native
surface binding or visible output. Missing association retains opaque rendering.
The colour route must not be resized or replaced with an atlas merely because
paired native coverage was requested.

The current software contracts are the [bridge protocol](protocols/stream-bridge-v1.md),
[session authentication](protocols/stream-bridge-auth-v1.md),
[sender host integration](research/sender-bridge-host-integration-20261018.md) and
[receiver host integration](research/receiver-hosted-bridge-20261018.md). Their
dated evidence does not certify an end-to-end headset quality/menu result.
The remaining atlas sections describe the independent stable fallback, not an
instruction to enable both formats for the same eye.

## Stable atlas architecture

```text
Supported game/mod, PC                       Steam Frame
current eye: RGB contribution E + coverage A
             │
        GPU atlas packing
             │
     ordinary SteamVR / Steam Link ───────► native video decode
                                             │
                                   existing native image + sampler
                                             │
                                   guarded native scene fragment
                                             │
                                  E + (1 − A) × local camera
```

The native camera never travels to the PC. This stable atlas path does not patch the PC
Steam Link sender, add a camera stream, or join a separately received “latest
mask” to a video frame. The game places both signals in one current per-eye
image. The receiver reads both through the original invocation's current image
and native chart, preserving the admitted native decode, reprojection,
distortion and foveation coordinate path.

That structural coupling does not authenticate the source. The format strip
recognizes a layout; a copied valid strip with an unrelated body can still pass.
Neither a submit counter, a timestamp nor the most recently observed descriptor
establishes an independent source session/frame/physical-eye association.

### Contribution is not straight RGB

The source supplies **E**, the RGB already contributed by the scene, and an
independent opacity/coverage **A**. For ordinary over, a correctly accumulated
source has `E = colour × A`; additive emission can have **E > 0 while A = 0**.
The receiver must retain that contribution:

```text
RGB output = native_decode(E) × original_tint × original_aperture
A output   = clamp(numeric_coverage, 0, 1) × original_aperture
```

Do not multiply E by A again, divide it by A, erase RGB at A0, or infer A from
RGB/key-colour distance. An RGBA render target alone does not prove that its alpha
has these semantics. Opaque shaders may write unrelated alpha; straight-alpha
blending applied to alpha can square it; additive particles, depthless early
backdrops and nested UI captures need their own rules.

This model supports fractional transparency with a correctly prepared source.
It does not supply refraction of the real room: a game shader that samples its
own scene background has not sampled the headset's camera.

## Frozen scene profiles

The current receiver recognizes **v3 and legacy v1**. V3 retains equal-resolution
colour/coverage and replaces the fragile format header. It requires a matching
updated producer and receiver. Both-eye presentation and corrected edges have
been confirmed on the headset, but a later direct comparison finds substantial
detail loss with MR enabled. See the [current quality investigation](research/stream-quality-comparison-20261018.md)
before treating the equal source allocations as preserved stream quality.
**V2 is quarantined after a failed stereo headset test:** lossy
transport can independently reject its small corner signature for one eye,
displaying the packed image at a different scale with opaque output. The exact
guard values from that hardware run were not recorded; a controlled codec plus
actual-fragment reproduction establishes the failure mechanism, not the real
encoder's parameters. See [the regression review](research/eye-divergence-20261018.md).
Do not use the idealized v2 GPU results as an end-to-end quality claim.

### Equal-resolution v3

The frozen contract is
[`scene-contribution-atlas-v3.json`](protocols/scene-contribution-atlas-v3.json).
Its canonical SHA256 is
`7efb2d98cc99d17d9bd920780797b6b647bc4b74441d2b97f8e72dc61f002521`;
its file SHA256 is
`cf5f71fb00877573a9d2aa8b69405ec7af03523563e216b4e07738605a11863a`.
Canvas, scene, coverage, windmill origins/rotations, global halos, contribution
and transfer are identical to v2 below. Only the format header changes:

| Field | Profile v3 |
|---|---|
| Schema / profile | `ghvr-steamlink-scene-atlas-v3` / `scene-contribution-atlas-v3`, revision 3. |
| Cells | Twelve128×128 neutral-grey squares, with32-pixel gaps, outside scene and all coverage halos. |
| First six origins | Row-major `(48,64)`, `(208,64)`, `(368,64)`, `(48,224)`, `(208,224)`, `(368,224)`. |
| Last six origins | Row-major `(2672,2704)`, `(2832,2704)`, `(2992,2704)`, `(2672,2864)`, `(2832,2864)`, `(2992,2864)`. |
| Levels / codes | Levels `32,96,160,224`; ordered codes `32,224,224,96,96,160,96,224,160,224,96,96`. |
| Admission | All twelve raw-G values within inclusive ±16 codes; no opacity threshold. |
| Derivation | First twelve SHA256-preimage bytes, each masked with3, index the four levels. |
| Guard signature | `681763eda11a55db2e43d18d52bf791a9c41720840471c0f9e473f71934786b9`. |

For signature derivation remove `signature_preimage`, `signature_sha256` and
`codes` from the guard of a complete copied profile. Canonicalize the remaining
JSON with sorted keys and compact separators, prepend
`GloomhavenVR/SteamLink/scene-contribution-atlas-v3\n`, append `\n`, and hash.
These deterministic codes recognize the format, not an authenticated scene.
Large flat cells intentionally cannot prove exact atlas registration: small
translations can leave every sampled center inside its square. Correct original
R1/image/eye binding remains a separate obligation.

`transform_robust()` and the public package generator recognize v3 first, then
the complete frozen v1 guard. Accepted v3, legacy v1 and ordinary first-mismatch
paths retain the twelve/thirteen/two guard-fetch budget. There is no third guard,
cached verdict, cross-eye decision or extra frame queue. Historical
`transform_dual()` continues to generate its original v1/v2 bytes for regression
controls; it is no longer the package generator's selection.
The [bounded v3 implementation record](research/robust-native-coverage-20261018.md)
separates actual-producer/lossy-codec/native-GPU checks from the pending headset
result, and records the required old-v2 stereo failure control.

The retained experimental contract is
[`scene-contribution-atlas-v2.json`](protocols/scene-contribution-atlas-v2.json),
whose canonical-profile SHA256 is
`68ad7a14b094729faffb3ff23cc5b42b2605ae9535e28d55071cd76533aecfc5`.
The pretty-printed file SHA256 is
`9ea2c9cd304a296d088659e3bc8e9a316365c8b10e3dfddb459bad08ff11ce9f`.

### Experimental equal-resolution v2

| Field | Profile v2 |
|---|---|
| Schema / profile | `ghvr-steamlink-scene-atlas-v2` / `scene-contribution-atlas-v2`, revision 2. |
| Logical canvas | 3168×3168; actual XR/video dimensions remain unchanged. |
| RGB contribution | Rectangle `(560,560,2048,2048)`. |
| Coverage field | 2048×2048, numeric **G**, four 2048×512 strips. Sample the same source RGBA grid as RGB, without an additional coverage box or reconstruction kernel. |
| Strip data origins | `(576,32)`, `(2624,576)`, `(544,2624)`, `(32,544)`. |
| Rotations | Strips 1 and 3 are quarter-turned. In texel-center index coordinates, map `(sourceX, localY)` to `origin + (localY, 2047-sourceX)`; other strips use `origin + (sourceX, localY)`. |
| Halos | 16 logical pixels continuing the global mask, with no overlapping outer rectangles. |
| Guard | Twelve 32×32 neutral cells, origin `(64,64)`, diagonal stride `(36,36)`. |
| Guard codes | `104,14,149,250,167,249,185,25,121,144,38,152`, tolerance ±2 codes. |
| Guard signature | SHA256 `680e95faa7f9b919799026984e1950f9ec2677b31901843f7ac1e78af1913e73`. |

Coordinate, RGB-contribution, transfer and current-image rules remain unchanged.
Larger coverage allocation trades away RGB area at a fixed carrier resolution;
it does not increase the user's resolution or bitrate. Genuine source MSAA,
partial opacity, gradients and positive RGB at A0 remain valid. No object/window
classification or opacity threshold is introduced. Desktop raster and native GPU
evidence is recorded in the [focused v2 report](research/full-resolution-native-coverage-20261018.md);
headset/codec quality and performance remain separate verification boundaries.

Historical `transform_dual()` tests the complete v2 guard first, then the
complete v1 guard. If neither matches, it retains the exact original output.
Single-profile `transform()` still defaults to unchanged v1. Neither historical
entry point is permission to select the quarantined v2 producer.

### Legacy v1

The retained normative v1 contract is
[`docs/protocols/scene-contribution-atlas-v1.json`](protocols/scene-contribution-atlas-v1.json).
The canonical-profile SHA256 is
`2c1ca215715ec7d2d7591eb2a2e8071a15e11fb5f96530927d10ceab7405e30c`.
The pretty-printed file and the canonical JSON serialization have different
hashes; use the generator's exact canonical admission rather than comparing an
arbitrary reserialization.

| Field | Profile v1 |
|---|---|
| Schema / profile | `ghvr-steamlink-scene-atlas-v1` / `scene-contribution-atlas-v1`, revision 1 |
| Coordinates | Top-down source texel centres, mapped through the current original R1 chart. |
| Logical canvas | 2112×2752. This does not enlarge the actual XR/video carrier. |
| RGB contribution | Rectangle `(32,352,2048,2048)`, centred vertically. |
| Coverage field | 1024×1024, numeric **G**, four 1024×256 row tiles. |
| Tile data origins | `(16,48)`, `(1072,48)`, `(16,2448)`, `(1072,2448)`. |
| Tile outer rectangles | 1056×288 at `(0,32)`, `(1056,32)`, `(0,2432)`, `(1056,2432)`. |
| Halos | 16 logical pixels, continuing neighbouring **global** mask rows; clamp only the complete mask's outside edge. |
| Filter | Average four fixed 2048-scene-grid samples into each 1024 mask cell, then reconstruct globally with bilinear filtering. |
| Guard | Twelve 32×32 neutral cells, origin `(64,2400)`, horizontal stride 160. |
| Guard codes | `111,208,128,104,227,251,255,193,102,168,15,90`, tolerance ±2 codes. |
| Guard signature | SHA256 `6fd08068e3fbffc166a80f5a1044d4c8135b37087441aac85f6223c121fccc5e`. |
| Initial transfer | Gamma working space, SDR UNorm source, no atlas sRGB conversion. |

The footer is **Y2400**, not the historical draft's Y2720. Duplicating each tile's
own edge instead of continuing global neighbours changes the filter at seams.
Changing a coordinate, convention, guard or filter requires a new explicit
profile and matching source/receiver tests; do not relabel incompatible content
as v1.

Lower-resolution coverage is an explicit quality compromise. Atlas resampling,
video encoding, neutral-field reconstruction and foveation can further affect
edges. The guard's twelve large neutral cells are format recognition, not
per-pixel opacity or colour-keying. A changed/frozen body with the same strip is
not detectable as stale by this guard.

## Native receiver and lifetime boundaries

The optional receiver is built through the historical
`FRAME_PIXEL_SYNTHETIC_PRESENTER` switch. Its name and existing internal
`GHVR`/`MRMASK` signatures are compatibility identifiers, not a promise that the
current game path is a synthetic pattern. Renaming the public repository must
not change those frozen bytes accidentally. The installed
`SteamFrameNativePassthrough` directory name also stays stable across branding
changes so existing installations remain manageable.

At an original successful `vkCreateGraphicsPipelines` callback, the receiver has
the actual still-live creation inputs. It verifies the original shader pins,
device API, descriptor/layout/render-pass definitions and accepted output state.
An admitted creation builds a private fragment module and one replacement
pipeline from the original creation info, changing only the fragment stage.
It returns the new normal pipeline handle and destroys the never-exposed old
pipeline. The application owns the result through its normal lifetime. Temporary
shader modules are destroyed after synchronous pipeline creation.

There is no per-draw pipeline swap, new mask descriptor set, SSBO, retained old
command-buffer lease or additional rendering queue in this presenter path.
Unknown layouts, unsupported blend/output state, unrecognized stages or failed
creation retain the original result. A recipe from an earlier capture or an equal
raw handle is not authority to alter a newly created pipeline.

Driver-specific private structure handling has a separate narrow capability.
Cold verification pins the actual saved downstream function pointers to the
accepted provider's file/mapped identity, build/hash, code segments and entry
offsets, with an unload-preventing lease. Callback use checks device, operation
and epoch through scoped permits. An arbitrary numeric extension tag or caller
boolean cannot mint that capability. Device close revokes it before joining the
cold worker and releasing the provider after callbacks quiesce.

File reads, ELF hashing, loader queries and artifact preparation belong to cold
setup. Render callbacks perform bounded state/atomic work. No shader compilation,
filesystem I/O, socket wait or image readback is allowed in a per-eye draw path.
Optional diagnostic collectors have separate explicit scopes and are not the
normal presenter or permission to capture images.

### OFF must work in reusable commands

The fragment examines the **current sampled image**, even when the application
replays an existing command buffer. Missing, invalid or out-of-profile guards
select the exact original output expression; they do not force a guessed alpha
or use cached coverage. The first-cell fast path reduces ordinary-image work.

Disabling the game's native source preference or Mixed Reality emits ordinary
game images. Opening VR Options does not disable the current real-game source;
settings apply while that menu remains open. Fallback applies when an ordinary
image is actually consumed. It cannot revoke a previously transmitted image,
detect a deliberately copied guard or promise an instant change before the next
image arrives. Disabling the receiver's installer
activation is different: already created pipelines remain until their process
exits, so a manual SteamVR restart is needed to unload them.

## Adding a game integration

GloomhavenVR is the only supported game integration. Its producer code lives in
the [GloomhavenVR repository](https://github.com/McFredward/GloomhavenVR), under
`Core/SteamLink`. Other games need a real producer, not just this headset install.

An integration must establish all of the following:

1. The current actual eye, extent and renderer/source shape. Do not infer an eye
   from alternating callbacks or borrow the preceding eye's image.
2. Same-eye contribution/coverage semantics across opaque/cutout depth and every
   admitted transparent/additive/UI/intermediate family. Preserve stencil,
   clipping, masks, fades, draw order and gameplay callbacks. Unreviewed material
   names or an alpha-capable allocation do not certify coverage.
3. Correct MSAA resolution, colour transfer and the frozen pixel-centre/filter
   rules. Retain additive E at A0. Do not make source texture resolution into a
   claim about the destination or encoded-video resolution.
4. Bounded ownership of the borrowed source through GPU submission, including
   explicit disable, camera replacement, unknown eye and renderer teardown.
   Keep menus interactive without implicitly disabling the source. Prepare
   materials/resources outside eye callbacks; do not add a previous-pair delay.
5. Original-output fallback on every rejected source path. Producer and receiver
   failures must not publish an old mask with a new eye image.

Conceptual flow, **not a generic API or an admission shortcut**:

```text
current actual eye callback:
    if source is disabled or current coverage is not established:
        submit ordinary current-eye output
    else:
        pack current-eye contribution E and coverage A into the exact v3 atlas
        submit that image through the ordinary game/SteamVR path
```

GloomhavenVR's reference uses the existing head camera's opaque depth attachment
for alpha-only reset/stamp commands, reviewed transparent-material rules, and a
same-eye GPU image effect. It adds no second scene camera or CPU pixel upload.
That is a scoped reference, not certification that all native game/UI shaders
already satisfy the contract. A source must also refuse unsupported renderer,
HDR/Linear, array/double-wide or unresolved attachment routes explicitly.

The old separate-mask UDP/session protocol remains an experimental foundation.
It is **not** a supported alternate transport for this installed presenter, and
its earlier marker/RGB conventions must not be substituted into the scene profile.

## Persistent activation and update boundaries

`setup.sh` owns a user-service drop-in and installed package/configuration under
the user's home. The public implicit-layer manifest points to a temporary
`receiver.json` in the service's per-user runtime directory. Pre-start preparation
publishes that target only after the current accepted file cohort matches.
Systemd removes the volatile target on service stop/reboot, including a case
where the Python preparation helper can no longer run. It must not leave an old
DSO automatically loadable merely because its installed files survived an update.
The installer never restarts the service itself.

An incomplete/abnormal prior launch disables later activation until explicit
review and `enable`. `emergency-off` removes activation without depending on a
working SteamVR API; restoration of the saved camera override is a distinct
operation and may remain pending. Preserve foreign files, changed user settings
and unrelated drop-ins instead of guessing ownership.

First install requests enabled state. An identical repeat preserves an explicit
manual disable. Live installation only stages the owned package/drop-in/link;
activation and camera changes wait for the next admitted launch. Updates require
a stopped SteamVR process and must not overwrite a mapped library. A live or
camera-pending uninstall retains a disabled stop helper/receipt until a later
stopped uninstall can finish; it must not report that an unlinked DSO has unloaded.
An old experimental installation is a hard collision requiring its original
rollback, never an automatically adopted package.

Frame-side file pins and PC-side stream compatibility are separate. A Windows
SteamVR/Steam Link update can change the stream without altering a single pinned
Frame file. The shader/interface/format guards are still needed after pre-start
admission. They do not provide a receiver handshake or turn the PC producer off.
Document the visible packed-atlas failure mode and source-off recovery rather
than promising that every rejected update remains visually undetectable.

## Build and validation

The user entry point is `bash setup.sh install`; it prepares a local checked
package before staging it. The native Linux/AArch64 preparation uses Python3,
`g++`, CMake, `spirv-val` and Vulkan headers; it does not require linking a Vulkan
development loader library. The separate internal entry point is
`python3 tools/prepare_native_release.py --output <new-private-package>`.
It checks the accepted native-file cohort before building and records the local
artifact identities for subsequent launch checks. It must never consume an
arbitrary supplied native shader or accept a user-typed checksum as executable
authority. Ordinary CMake configuration does not install a Vulkan layer or enable
a headset camera.

For a host development build with CMake, a C++17 compiler, Python3 and Vulkan
development files:

```bash
cmake -S . -B build/dev -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/dev --parallel 2
ctest --test-dir build/dev --output-on-failure \
  -R '^(native_presenter_pipeline|native_presenter_receiver|native_fragment_scene_presenter|scene_presenter_session)$'
```

These are focused software checks, not installation or a new complete gate.
Some positive native-shader fixtures additionally require private locally read
originals; public refusal fixtures must remain useful without them. Shader
assembly/validation and actual GPU fixtures require their declared SPIR-V tools,
Vulkan driver and validation-layer dependencies. Do not silently report a skipped
GPU/private case as a positive pass.

Conformance should include independent original-output/source-over oracles,
fractional opacity, E>0/A0, unequal native reconstruction weights, tile seams,
quarter/asymmetric charts and precise pixel-centre conventions. Same-recorded-
command ON/OFF/ON, first/later guard corruption, missing/partial/foreign formats,
wrong generation/provider/device and partial creation failures need negative
controls. A causal mutant must fail the intended oracle, not merely compile
incorrectly. CPU schema checks, owned host GPU evidence and visible headset
confirmation are different claims.

Run the affected subset after a bounded repair and preserve already passing
evidence for unchanged areas. Record source hashes, selected modes, skips and
failures honestly. After subprocesses stop, remove only regenerable owned build
caches; preserve small results, source/manifest pins, failures and current
deliverables. Use an existing harness's completion-retention API when supplied.
Never purge another worker's active inputs or shared game references.

## Contributing and evidence handling

- Branch from current `dev` in an isolated worktree. Coordinate disjoint file
  ownership and integrate through `dev`; the integrator publishes the reviewed
  result to public `main`. Do not push worker branches or rewrite retained history.
- Propose source/protocol changes with exact compatibility scope and original
  fallback. An unknown SteamVR/driver build needs reviewed admission, not a loose
  version comparison or a numeric whitelist.
- Keep private Valve binaries, original/derived shader executables,
  disassemblies, captures and home-camera images out of commits, releases and
  issue attachments. Generate accepted executable artifacts locally from the
  installed runtime. Small hashes and bounded source-derived facts can document
  evidence without redistributing those inputs.
- Report the game/mod build, installed project revision, `setup.sh status`,
  original-output/menu behavior and a bounded relevant log excerpt. Do not post
  credentials. Host software checks do not prove a headset picture.
- No developer test or documentation change authorizes a device installation,
  restart or camera-setting mutation. Hardware work needs its own concrete scope
  and recovery plan; avoid automatic restarts and expiry timers.

The [creation-scoped receiver note](research/native-presenter-creation-20261017.md),
[scene generator note](research/native-scene-presenter-20261017.md) and
[visible game result](research/real-game-visible-run-20261017.md) retain precise
checkpoint evidence. The [unaltered previous README](archive/README-before-native-passthrough-20261017.md)
preserves the earlier investigation chronology. Its original relative links and
dated status statements are historical, not the current installation guide.
