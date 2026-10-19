# Developing Steam Link Native Passthrough

Current development supports one **experimental paired Windows/NVIDIA/HEVC
route**. [Installation](INSTALL.md) and [compatibility](COMPATIBILITY.md) are the
player entry points. The [retired atlas record](history/ATLAS-RECEIVER.md) preserves
the earlier implementation and evidence; its formats are not runtime fallbacks.

## Architecture

```text
Game current eye: RGB contribution E + independent coverage A
  → original SteamVR per-eye shared texture
  → pinned Windows sender companion: native source and encoder-input proof
  → original colour HEVC access unit + authenticated coverage metadata
  → matching Frame companion: exact AU / decoded image / lease association
  → guarded native game composition over local Frame camera
```

The native camera never leaves the Frame. Colour is never used to infer opacity.
The game source retains its complete ordinary eye image: do not reserve video
space for a mask, resize its RGB region or install the old quality/foveation lease.
The intended composite is **E+(1−A)×camera**. E is already contribution, so do not
multiply it by A again. Positive RGB at A0 is valid additive emission.

## Integrate a game

1. Produce contribution RGB and genuine coverage alpha for the same current eye.
   Preserve opaque depth and account for supported transparent materials. An
   alpha-capable render target alone does not establish correct coverage.
2. Negotiate the supported ordinary-RGBA source capability with the local sender.
   Verify process identity, current source ownership, nonces, version and complete
   records. Do not treat headset identity or a saved option as patch readiness.
3. Apply source permission at a complete stereo boundary and check it again at
   the real rendering seam. Revoke on loss; never keep the second eye on an older
   source epoch. Missing authority retains ordinary opaque output.
4. Keep rendering controls reversible. Toggling this feature must not change the
   player's eye resolution, MSAA, field of view or unrelated Steam Link settings.
5. Test both eyes, loading, render-target/source replacement, reconnect, menus,
   transparency, option changes and teardown. Separate software proof from what
   was actually seen on a headset.

See [GloomhavenVR's source contract](https://github.com/McFredward/GloomhavenVR/blob/dev/docs/STEAM-LINK-COVERAGE-SOURCE.md)
for an integration example and its game-side code. Other games need their own
source integration; installation alone cannot infer transparency in an unmodified game.

## Identity and admission contracts

- Local source capability and operational coverage are different states. The
  format grant must not wait for the first mask if that would prevent the game
  from supplying its first ordinary-RGBA frame.
- The sender checks the exact version-pinned DirectMode component, owning game
  PID, shared allocation handle, swap generation, submitted bounds/projection,
  current eye/layer and accepted encoder-input identity. Packed native descriptor
  fields are decoded only according to the pinned ABI; the descriptor passed to
  SteamVR remains byte-for-byte unchanged.
- A mutually confirmed authenticated session is required. The same HEVC access
  unit carries its original-video digest and independently certified coverage;
  metadata must match that exact accepted submission and encoder generation.
- The Frame joins successful native decoder observations, current DMA-BUF kernel
  identity, allocation/capture generations and revocable leases to the actual
  selected sampled image. A compositor counter, approximate timestamp, submit
  completion or separately received latest mask is not this proof.
- Missing proof retains ordinary RGB. It must not select an atlas decoder,
  replace a colour with transparency or reuse an earlier image's mask.

Detailed contracts: [bridge](protocols/stream-bridge-v1.md),
[authentication](protocols/stream-bridge-auth-v1.md),
[sender integration](research/sender-bridge-host-integration-20261018.md) and
[receiver integration](research/receiver-hosted-bridge-20261018.md).
Dated research reports describe their own checkpoint; archived fallback statements
there do not authorize an active atlas path in current code.

## Native rendering

Keep the original RGB draw and native reprojection, distortion, foveation mapping
and UI composition. The companion's optional coverage work uses exact admitted
resources; restore all changed native state. Render callbacks must not perform
file I/O, socket waits, shader compilation or unbounded work. Metadata contention
and resource/permission failure must fail opaque without blocking native rendering.

Native D3D dispatch may mutate after Draw/Copy/readback. The object-local writable
facade therefore forwards through live original targets, retains the complete
public context ABI and keeps every published callback/table owner alive. It does
not patch system DLL instructions or reinstall hooks periodically. Exact return-
address and presentation-scope checks remain independent of dispatch diagnostics.

The Frame's replacement coverage operation is admitted only after exact shader,
descriptor, source-image and selected-frame contracts match. Unsupported native
pipelines retain their original handles/output. Local Steam UI must keep its
native content; final game/menu visual composition remains a hardware milestone.

### Camera lifetime and standby

Installation must not leave `camera/forceSceneAlphaBlending` permanently enabled.
Only authenticated current-stream Raw/RLE coverage may request this setting.
The compositor's cold worker uses an isolated Python background application and
the pinned public native settings interface; never initialize or shut down
OpenVR inside Valve's Steam or compositor process. The renderer and decoder
handoff require the helper's current enable acknowledgment, independently of
the exact image/mask proof.

Each native command recording freezes one nonzero camera grant. Both eye draws
and submission require that exact grant; readiness becoming true halfway through
a recording must not activate only one eye. A revoke followed by a fresh enable
gets a different grant, so an old recording cannot regain permission by ABA.

None coverage, source-epoch loss, disconnect and teardown revoke readiness and
restore the original preference. A real Linux suspend is detected by the
`CLOCK_BOOTTIME − CLOCK_MONOTONIC` offset, not by a long frame or an expiry timer.
Resume requires an acknowledged restore followed by a new enable acknowledgment,
even when the peer, video allocation and saved MR choice have not changed.

Durable private leases retain the original key before enabling it. A fail-open
SteamVR pre-start helper repairs only an exact stale owned lease while native
VR is stopped. Unknown/user-modified values are preserved. Old permanent-True
installations get a one-use receipt-bound migration grant. Update, uninstall
and Undo quiesce the live helper and confirm native persistence before deleting
its files; offline file replacement is never substituted for a live settings
API call. See the [camera and continuity review](research/camera-lifetime-and-continuity-20261019.md)
for the evidence and remaining hardware checks.

## Packaging and updates

The guided [PC + Frame assistant](PC-FRAME-SETUP.md) is the sole install workflow.
It ships reviewed owned binaries, source/tool hashes and runtime fingerprints;
Valve binaries, disassemblies and native-derived shader output are not redistributed.
Generate the admitted coverage shader locally from the user's checked runtime.
Bundle parsing, transitive generator pins, artifact hashes and release validation
receipts must agree before a package can apply changes.

Default and preview bootstraps select the same paired schema. Inspection and
legacy-removal management cannot install a receiver or grant source permission.
A preserved old receipt exists only for migration/removal/Undo. Never generate
or activate an old atlas shader during a new installation or compatibility refusal.

Stage files while the Frame's current UI remains running, then perform only the
user-confirmed coordinated service transition. Startup monitoring must inspect
fresh processes/logs and retain the user's visible-picture check. Undo is complete
only after the previous owned state and running Frame UI are confirmed. Ctrl+C
is an explicit abort without an automatic rollback/restart; retain recovery records.

## Evidence and development discipline

Use separate worktrees and current dev ancestry. Run affected production-path
fixtures and meaningful causal controls; reuse passing evidence for untouched
areas. Scope the actual Windows tests separately from portable boundary tests.
Retain small source/artifact/result pins and remove completed generated caches.
No checked-in Valve shader, private native PE, disassembly or device credentials.

The old route's visible gameplay does not certify the new full-resolution output.
Current [compatibility](COMPATIBILITY.md#current-evidence) distinguishes confirmed
native hooks/certificates, software checks and pending visual hardware outcomes.
