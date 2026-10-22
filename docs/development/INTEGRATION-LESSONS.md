# Integration lessons from GloomhavenVR and Demeo

Read with the [integration guide](GAME-INTEGRATION.md). These are reusable
rendering/transport lessons from the October 2026 integrations, not a promise
that another game's ABI, materials or timing will match. Dated investigation
reports remain evidence for their checkpoint, not current activation recipes.

## Alpha survives the whole route

GloomhavenVR owns its VR camera and can establish a coherent output early.
Demeo already owns its Unity 6 URP/XR pipeline, Linear lighting, array stages,
post-processing and loading cameras. Changing one camera clear colour cannot
establish coverage through all of that. Existing OpenVR and OpenXR games both
work; the API name alone does not identify the alpha defect or its solution.

Trace these seams in order: camera/depth -> opaque pass -> transparents ->
post-processing -> resolve -> XR Submit -> SteamVR source view/copy -> native
mask -> accepted encoded AU -> selected Frame image. Compare both eyes and
retain exact source/format/stage identity at each seam.

| Symptom | What to check before changing shaders |
|---|---|
| All solid objects look partly transparent | Final coverage versus material alpha; post-processing/resolve/filter alpha; colour-storage declaration; selected mask identity |
| Different opacity or missing content in one eye | Real render passes, array slice/stage, bounds and source epoch; independent native eye identity |
| Only passthrough or an entirely black right eye | Missing or zero-alpha eye data, unsupported shader loading, lost wrapper/pipeline, incorrect generated IL2CPP ABI |
| Pixelated menu with unchanged resolution | Video budget after metadata reservation; dense fractional mask content; actual streaming metrics before assuming foveation changed |

The game alpha is coverage A; composition is E+(1-A)*camera. Transparent
materials need genuine fractional coverage. Alpha=0 does not imply RGB=0:
additive light can retain positive contribution. If a product wants glow to
occlude the room, preserve its chosen coverage rather than globally making
all light transparent to save bandwidth.

Unity's Linear lighting space and SDR storage transfer are separate facts.
Demeo keeps Linear lighting and already encoded sRGB RGB. Compatible per-slice
copies into UNorm storage with an explicit OpenVR Gamma declaration preserve
those bytes. Do not switch the whole project to Gamma or double-encode RGB.
URP may expose alpha-processing settings while its released player has stripped
the corresponding shader variants. Demeo checks alpha-capable intermediates,
owns a reversible post-processing bypass and repairs alpha on the actual
current opaque/depth attachments before transparents.

One Demeo investigation also found SteamVR's optional supersample-filter copy
path relevant to preserving source alpha. Its adapter scopes a checked
`IVRSettings_003` query override for exactly `steamvr/allowSupersampleFiltering`
to the same thread's admitted original Submit and live grant. This is not a
global settings write or an instruction to disable foveation. The source and
native ABI tests must be understood before considering it for another game.

Sparse alpha probes are diagnostic samples, not whole-object proof. The earlier
Demeo runs sampled alpha255 at game Submit yet received transparent masks; that
was evidence to inspect downstream conversion, not to set every object alpha1.

## Match the actual stereo provider

Trying to split one native two-view URP pass into two invented passes caused
right-eye loss and flicker. Explicit array indices alone also did not establish
correct final eye alpha. Demeo/Battles now preserve their original provider
layout and use current resolved slices with independent 2D OpenVR stages.
Hold every target until its native use is finished; a Unity temporary texture
released after Submit is not automatically safe for an asynchronous consumer.

Battlemarked's existing OpenXR array route exposed a compositor-owned
intermediate instead of the declared game's native source. Its separate fix
uses the validated public OpenXR render-mode API and waits for two real 2D
passes. Do not copy this as a blanket XR restart or forced mode toggle for
every Unity game. Prove the new actual layout before granting source output.

## Load shaders through the actual runtime ABI

Managed fixture success does not execute native IL2CPP detours. Unity 6's
generated AssetBundle overloads and struct-return/out wrappers caused real
failures despite successful compilation. Demeo's IL2CPP shader loader validates
native byte/UTF16 spans, resolved entry points and returned Unity GC handles.
Owned bundle/shader lifetimes must survive scene cleanup where required and be
released at explicit unload. Retain delegates/native callbacks until fetched
callers retire. Do not copy a Unity-version-specific injected-call signature
into a new game without validating its ABI.

The optional IL2CPP XR GPU diagnostic was disabled where its generated out
binding was unsafe. Production coverage and exact-submit probes remain usable;
do not let an optional diagnostic crash the game it is meant to inspect.

## Inventory every source of authored black

Intro sphere/Timeline materials, camera fades and loading Canvas backgrounds
were independent causes. Fixing one did not fix the others. A first-frame
black loading label/background also survived fixes applied only on Update.
Initialize the identified material/UI state before its first draw and preserve
fade timing, callbacks, input blocking and useful loading content. Match exact
known components/materials, preserve animated alpha/property blocks, and restore
owned changes on opt-out/unload. Do not classify all black pixels or hide all UI.

Scene replacement is another boundary: preserve only authenticated source intent
through a checked camera-less handoff. An unexpected new camera drawing during
the handoff must revoke the old grant. Avoid a two-second retry backoff after a
known short source renewal; retry policy is cold application work, not a timer
that periodically expires healthy native permission.

## Bandwidth is RGB video plus coverage

Lossless full-resolution coverage has a cost even though the room camera stays
local. The native mask follows the coded video's mapping. Soft fog, particles
and long light shafts create many fractional samples, which can be expensive
to encode without changing a single visible RGB object's shape.

Battlemarked metadata reservations exceeded500 Mbit/s in one heavy menu capture
and left video at its1 Mbit/s floor; packet/FEC failures and decoder resets were
observed. The v2 wire accountant reserves mask traffic within the current native
rate/VBV budget for both Auto and fixed-rate requests. v1 sources retain their
rate policy; the negotiated lossless codec/recovery improvements apply to both.
The encoder selects independent Raw/RLE/LZ4/Zstd representations under bounded
sizes and work. Do not reduce mask precision or native mapping to hide overload.

After removing only the longest menu ray in the separate game mod, sampled
mask payload median fell about23% and video-floor samples fell from3/9 to0/8.
Mean metadata reservation barely changed (about125 to124 Mbit/s). Different
views and Auto bitrate mean this is not a controlled per-effect or total-link
bandwidth measurement. Do not assign unsupported percentages to fog versus
light shafts. Keep effect removal scoped, reversible and user-authorized.

See [codec/recovery](NATIVE-MASK-CODEC.md) and
[wire budget](BATTLES-BURST-BUDGET.md) for the exact policies and older evidence.

## Recovery and desktop are different picture policies

The active-MR design is camera-only output during missing exact game data, then
resumption with the exact current mask. Reusing the last mask, guessing from
RGB or disabling the camera for each empty metadata packet causes incorrect
composition or visible black transitions. Decoder renewal invalidates image
leases while preserving authenticated recovery intent; a genuine local channel
loss or explicit source withdrawal is a separate revocation.

The first recovery implementation hid the desktop while its mouse still worked.
The corrected route authenticates policy3 for a completely proved ordinary
native picture, including its copy, accepted encoder input, matching output and
selected decoded-image lease. It needs no game mask or camera acknowledgment;
the physical camera remains warm. Future desktop metadata, a foreign PID alone
or a missing game mask cannot exempt the next game picture. Returning to the
game immediately requires its own exact coverage again.

A wrapper bug also rejected the valid mask-independent recovery fragment when
set2 was present without mask set1. This marked native tracking lost and closed
the receiver channel. Isolated renderer tests and offscreen shader readback had
passed. The fix required exercising the actual wrapper, original-pipeline
restoration and subsequent mask-enabled rendering. A pre-main worker lifetime
issue was separately repaired. Keep those integration tests when changing the
wrapper or worker ownership; do not weaken shader/source checks to hide failure.

The [desktop note](DESKTOP-RECOVERY.md) and signed
[coverage policies](../protocols/stream-coverage-v1.md) are the current contracts.

## Capture useful evidence

Use dev/Debug diagnostics for investigation; main/Important deliberately omits
routine counters. Correlate PC and Frame times, process/session identity and
counter deltas within the actual test. Logs can include previous games and
earlier process lifetimes. Lifetime totals are not proof of a new test's frames.

Read source refusals, final-eye alpha samples, mask bytes/encoding, rate budget,
FEC/backtrack/decoder events, authenticated mask rejects, upload/submit refusals,
camera grants and pipeline loss together. A SourceIpc format grant, a zero-error
Submit, an accepted packet and a user's headset picture prove different things.
One initialization decoder reset does not establish a mid-game reset. Neither
clean counters nor a passing fake can prove every headset frame was correct.

## Source map

| Task | Current reference |
|---|---|
| Kernel-checked game lease | [Source IPC](../../pc/stream-bridge-sender-provider-sourceipc.h), [source contract](../../pc/stream-bridge-source-contract.h) |
| Native exact copy/encode ownership | [Coverage stages](../../pc/stream-bridge-sender-provider-coverage-stages.h), [NVENC boundary](../../pc/stream-bridge-sender-provider-core.h) |
| Authentication/codecs/policies | [Protocol header](../../bridge/stream_bridge_protocol.h), [signed frame implementation](../../bridge/stream_bridge_frame.cpp) |
| Decoded handoff, recovery, rendering | [Process](../../layer/stream_bridge_process.cpp), [renderer](../../layer/stream_bridge_renderer.cpp) |
| Demeo game rendering and tests | [Demeo repository](https://github.com/McFredward/demeo-mixed-reality), [integration evidence](https://github.com/McFredward/demeo-mixed-reality/blob/main/docs/STEAM-LINK-INTEGRATION.md) |
| Demeo adapters to inspect | `NativePassthrough.cs`, `OpaqueCoverage.cs`, `SrpOpaqueCoverage.cs`, `OpenVrEyeTextures.cs`, `OpenVrSubmitSettings.cs`, `OpenXrSubmission.cs`, `UnityShaderLoader.cs`, `PassthroughFade.cs`, `LoadingPassthrough.cs`, `SteamLinkCoverageBridge.cs` in that repository |
| Gloomhaven source and camera ownership | [Source contract](https://github.com/McFredward/GloomhavenVR/blob/dev/docs/STEAM-LINK-COVERAGE-SOURCE.md), `src/GloomhavenVR/Core/SteamLink/SceneEyeCoverage.cs` and `SteamLinkCoverageBridge.cs` |

Copy rendering concepts and tested lifecycle boundaries, not unrelated game
behavior or private files. Preserve upstream licences and attribution when
reusing code. Keep game-specific adapters in the game's repository.
