# Integrating another PCVR game

This is the handoff guide for a game-mod agent. Read it before the older
investigation logs. The released baseline is installer 0.2.22, PC 0.2.21 and
Frame 0.2.18. Read the repository's current version files rather than using
these numbers as an activation allowlist.

## Read these first

1. This guide and [integration lessons](INTEGRATION-LESSONS.md).
2. The [application source API](../protocols/application-source-v1.md), including
   its optional v2 budget. This is the interface a game normally implements.
3. [Compatibility and evidence](../COMPATIBILITY.md) and
   [desktop/recovery behavior](DESKTOP-RECOVERY.md).
4. [Build/check commands](BUILDING.md) for any companion work actually needed.

The supported host route is Windows x64, NVIDIA, HEVC, SteamVR and Steam Frame
over Steam Link. Another PC GPU/backend, Linux SteamVR or another headset is
a backend port, not something a new game declaration alone enables. Keep a
game's existing Virtual Desktop colour-key mode separate from native mode.

## What the game owns

```text
Game: same current eye's RGB contribution E and coverage alpha A
  -> original SteamVR submission and source texture
  -> PC companion: exact native source/copy/accepted encoder-input receipt
  -> original HEVC access unit plus authenticated lossless coverage metadata
  -> Frame: matching decoded-image lease and native selected-image proof
  -> native composition: E + (1 - A) * local camera
```

The game implements source format, alpha, lifecycle and identity gating. Reuse
the paired companions. It does not send masks over its own UDP stream, upload
camera video, invent an encoder frame number or patch the Frame per game.
Do not change the shared companions merely to compensate for a game-side alpha
or stereo problem. General companion improvements must also preserve existing
v1/Gloomhaven and v2 sources.

## 1. Inventory before choosing an adapter

Record these facts from the actual installed game and its runtime logs:

| Question | Evidence to retain |
|---|---|
| What runs? | Executable, Steam app ID, actual game PID, engine/version, Mono or IL2CPP, loader/version |
| What renders? | Built-in/URP/other pipeline, graphics API, project lighting space, final eye storage/colour declaration |
| How are eyes submitted? | OpenVR or OpenXR, real display render-pass count, parameters, array slices, stages, bounds, samples/resolve |
| Which camera is current? | Active stereo game camera, camera stacks, scene replacement and destruction paths |
| What covers the room? | Opaque depth, transparents/particles, glow, UI, post-processing, authored fades and loading canvases |
| Where can alpha change? | Final render pass, resolve, XR plugin submission, SteamVR copy/filter, native mask extraction |

Capture baseline startup, menu, loading and gameplay. Obtain the correct loader
and generated interop references for IL2CPP. Keep game assemblies, runtime
binaries and captures private; distribute the mod and metadata-only build
references, not executable game code. Do not assume another Unity title has the
same shader variants, struct-return ABI or camera lifecycle.

## 2. Produce actual eye coverage

- Clear empty background to A=0. Fully opaque geometry must contribute A=1.
  Correct opaque depth coverage before transparent rendering if the game's
  opaque shaders leave alpha undefined. Use the actual current eye colour,
  depth and MSAA attachments, not another camera or previous depth.
- Preserve legitimate fractional alpha and positive additive RGB. E is already
  RGB contribution; multiplying it by A again darkens the picture. Decide how
  the game's glow should occlude the room explicitly; do not globally rewrite
  all additive materials or threshold every nonzero alpha to opaque.
- Check the final submitted image, not just a camera intermediate. An RGBA
  format or a few alpha=255 probes does not establish whole-scene correctness.
- Keep the game's lighting space. Demeo's Linear project retains its normal
  sRGB encoding; its OpenVR exchange uses compatible UNorm storage with an
  explicit Gamma declaration. Do not relabel arbitrary unknown inputs or
  apply another gamma conversion. Preserve every RGB/alpha byte during copies.
- Preserve resolution, FOV, MSAA and native foveation. Internal HDR processing
  may need an alpha-capable intermediate; that does not admit HDR headset output.
  An RGB-only HDR intermediate can discard alpha before the final RGBA target.
  [Lessons](INTEGRATION-LESSONS.md#alpha-survives-the-whole-route) describe URP
  alpha-output variants and the scoped OpenVR filter issue.

## 3. Establish two coherent eyes

Inspect the actual XR provider layout. An engine flag saying MultiPass is not
proof of independent native eyes. Both eyes must use the current source epoch,
correct array slice/stage, original bounds and the matching resolve.

For an existing OpenVR two-slice provider, Demeo preserves the provider layout
and copies each current resolved slice into its corresponding held 2D stage
target immediately at Submit. It retains native lifetimes until GPU use ends.
For Battlemarked's OpenXR provider, the checked public render-mode API requests
MultiPass and admission waits for two actual independent 2D display passes.
These are different verified adapters, not a universal OpenVR-to-OpenXR switch.
Never fabricate an extra URP pass over a provider's single two-view pass, restart
the XR display speculatively, or use the last successful left eye for the right.

## 4. Negotiate source permission off the rendering thread

Implement the exact [128-byte SBN1 contract](../protocols/application-source-v1.md).
Discover the real SteamVR host, verify the named pipe server's kernel PID and
canonical runtime identity, and let the provider verify the client's kernel PID.
Validate version, flags, profile, nonce, binding, generation and reserved bytes.

Prefer v2 when mask-bandwidth accounting is wanted. An older provider may
reject it; a single fresh v1 connection with a new nonce is the compatible
fallback. Do not keep v1 and v2 observers alive together. A healthy live source
lease has no periodic feature-expiry timer.

`canAcceptOrdinaryRgba` is source-format permission. `coverageOperational` is
not a prerequisite for rendering the first RGBA eye pair; the provider does not
emit that flag as proof of a displayed picture. Waiting for it creates a
startup deadlock. Authentication and exact encoder/decoder/image proof remain
the companions' responsibility, independently of the game capability reply.

Apply changes at a complete stereo boundary and recheck at the real render seam.
Keep registry/process/pipe I/O, reconnection, compilation, shader loading and
allocation on cold paths. Retain callbacks/delegates and native resources for
every in-flight caller; schedule cancellation/disposal without blocking XR.

## 5. Handle startup, scene changes and authored black

Require the player's native option, positive Steam Frame/Steam Link identity,
the game's actual transport and a supported current render profile. A headset
model string alone is insufficient. Preserve ordinary VR/colour-key behavior
outside native admission and restore only changes still owned by this adapter.
Avoid showing the legacy green background before native identity has settled:
prepare the intended opaque startup background before its first render.

Inventory each fade separately: intro Timeline materials, camera fades, screen
overlays and loading UI can all write opaque black after otherwise correct
scene alpha. Redirect identified black fades to camera-visible output while
preserving timing, loading completion, input blocking, useful text and artwork.
Initialize the correct state in the constructor/first render, not a later tick.
Never make every dark game pixel transparent.

Track late-created cameras and resource replacement. Revoke old eye/mask
authority at replacement; avoid an artificial retry delay between healthy
successive source connections. A short camera-less handoff can preserve source
intent only under checked identity and a bounded lifecycle; no new/unadmitted
camera may render under the old permission. Missing exact game data during
active authenticated MR is handled by camera-only recovery, not an old mask.

## 6. Preserve overlays and diagnose bandwidth

Test opening the Steam overlay, displaying/using its desktop screen, and
returning to the game. The shared desktop policy keeps the camera warm while
an exactly proved ordinary decoded picture bypasses MR coverage. A missing
mask is not evidence that an image is a desktop. Do not implement a per-game
overlay visibility toggle or weaken the native image proof.

Use v2 metadata accounting with Auto and fixed bitrate. Look at mask bytes,
fractional samples, reserved metadata and video requested/passed rates together.
Dense fog and long soft light shafts can make a full-resolution lossless mask
expensive even when RGB looks simple. Preserve native mapping and alpha
precision. Remove effects only as an explicit, scoped product decision; measure
them separately and retain flames, lighting and scenario effects otherwise.

## 7. Validate and hand off

| Boundary | Required checks |
|---|---|
| Game client | Real parser/lifecycle tests: malformed/partial reply, wrong PID/nonce/version, EOF, opt-out, renewal, one observer, no bootstrap dependency on operational coverage |
| Rendering | Both current eyes, opaque/fractional/additive coverage, late replacement, changed stage/extent, source loss between eyes, opt-out/unload restoration |
| Unity/loader | Compile against each actual supported loader; test native ABI/struct returns and shader loading separately from managed method fixtures |
| Lifecycle | First startup, intro/skip, menu, loading in/out, gameplay, pause, toggles, reconnect, standby/resume and teardown |
| Steam UI | Overlay desktop is visible and interactive; return immediately requires the game's exact mask again |
| Transport | Auto/fixed settings, metadata-heavy scenes and loss/recovery; distinguish game fades, packet loss and pipeline failure |

Use the [existing production fixtures](BUILDING.md) for a changed companion
boundary; a game-only integration normally needs its game tests and headset
run, not another native patch release. Shader-only or fake Vulkan checks missed
a real wrapper bug before: exercise the actual wrapper too when it changes.

Record exact mod/PC/Frame versions and revisions, SteamVR builds, render layout,
bitrate setting, test times and matching PC/Frame logs. State separately what
software proves, what the user saw, and what remains untested. Read Frame logs
only with the user's authorized access; an agent's investigation never implies
permission to install, write settings or restart the headset. Never include
device credentials in docs or logs. Keep user README instructions short.

## Copyable task for the next agent

```text
Integrate <game and repository> with Steam Link Native Passthrough.
Read AGENTS.md, docs/DEVELOPING.md, docs/development/GAME-INTEGRATION.md,
docs/development/INTEGRATION-LESSONS.md and the application-source API first.
Game files/interop/logs: <private input locations>; loader: <known or inspect>.
Inventory the actual engine, graphics API, XR layout and source ownership.
Reuse the paired route and the current game integrations; implement genuine
same-eye RGBA coverage and a kernel-verified live source lease. Keep lighting,
fractional/additive effects, native RGB/foveation and existing games working.
Cover initial loading/fades, scene replacement, overlays and source recovery.
Use optional v2 budgeting without making v1 or operational-coverage bootstrapping
a requirement. Do not copy historical atlas activation or blanket camera hacks.
Run affected checks and provide exact builds, bounded diagnostic logs and a
headset test checklist. Label software versus observed hardware results.
Work on dev with the maintainer's Git identity. Do not publish main/releases
or change/restart the Frame without the corresponding user authorization.
```

Working source examples are linked from the [lessons](INTEGRATION-LESSONS.md#source-map).
