# Compatibility and current limits

Steam Link Native Passthrough currently targets one combination: a supported
PC game integration, ordinary Steam Link PC VR streaming, and a Steam Frame
runtime accepted by the local compatibility checks. It is not a universal
transparency switch for any streamed game.

## Supported combination

| Component | Current scope |
|---|---|
| Headset | Steam Frame; the native receiver runs on the headset. |
| Transport | Steam Link PC VR through SteamVR, with the ordinary PC sender. No sender patch or second camera video stream. |
| Game | Gloomhaven Digital with a GloomhavenVR build containing the native Steam Link integration. Unmodified clients do not supply the format. |
| PC graphics path | The visible game test used Windows/D3D11, Unity's built-in renderer, MultiPass, Gamma colour space and an SDR alpha-capable source. |
| Frame software | SteamVR **2.18.2 (build 1791331621, Linux ARM64)**, checked on 2026-10-17. Exact compositor shader, descriptor/state and driver-provider checks must also pass; a matching SteamVR/SteamOS version label alone is insufficient. |
| Other games/transports | Not supported by the current game integration. ALVR, Virtual Desktop and other headsets are not established by these tests. |

The receiver is distinct from GloomhavenVR's **Steam Frame standalone**
passthrough. It processes a PC VR stream; it does not install or configure the
standalone game.

The Frame-side version above does not establish a particular Windows SteamVR
version. PC-side stream changes have their own compatibility limits below.

## What has actually been shown

On 2026-10-17 the maintainer confirmed real GloomhavenVR gameplay composited over
the Frame's native passthrough camera, using ModBuild686. Application logs record
both 4040×4040 MultiPass/MSAA8 eyes. Earlier synthetic-source tests also showed
fractional transparency.

Those visible headset tests used an explicitly staged, temporary receiver for
one compositor process. The public persistent package was subsequently installed
on the Frame and loaded across two SteamVR service starts with fresh launch
claims; the second also passed the native UI startup health gate. This does not
establish a full OS reboot, a new streamed-game picture, every future SteamVR
update, every material or a measured end-to-end latency. See the dated
[real-game result](research/real-game-visible-run-20261017.md) and separate
[persistent-setup validation](research/persistent-user-setup-20261017.md) for
their respective evidence and limits.

## Picture and performance limits

- The source must supply RGB contribution and independent opacity for the same
  current eye. A texture with an alpha channel is not enough on its own.
- Fractional opacity is supported. Arbitrary refractive glass, effects that sample
  a different background and all transparent shader families are not certified.
- The updated receiver recognizes **v3 and legacy v1**. V3 uses equal2048² colour
  and opacity with a larger, codec-resistant format header. It removes the
  dedicated half-resolution coverage filter, without erasing genuine partial
  transparency. Both-eye presentation and corrected edges have been confirmed,
  but the latest direct comparison finds substantial detail loss with MR on.
  Preserving the normal stream's quality remains open; see the
  [current investigation](research/stream-quality-comparison-20261018.md).
  Legacy v1 retains1024² opacity versus2048² colour and remains compatible.
- Historical experimental v2 also uses equal2048² grids, but its first real
  stereo test failed: apparent scale and passthrough differed between
  eyes. Controlled lossy-codec inputs reproduce asymmetric guard rejection and
  that exact output split in the actual native fragment. The failing hardware
  guard samples were not logged, so the real triggering cell/codec settings are
  unproven. V2 is quarantined; its desktop sharpness results are insufficient.
  See the [regression review](research/eye-divergence-20261018.md). V3 changes
  the header, leaving v2's scene/mask geometry unchanged. Both producer and
  receiver must be updated together for v3. A new producer with an old receiver
  can show a packed picture instead of the scene; disable native streaming in
  the game until the receiver update is complete. Legacy v1 does not require v3.
- Motion latency and the cost of extra image packing/sampling remain relevant.
  Host GPU measurements are not a Frame performance guarantee. There is no extra
  previous-eye/pair queue in the GloomhavenVR source helper, but the ordinary
  rendering, video, reprojection and display pipeline still has latency.
- HDR, Linear colour space and unsupported stereo/source shapes keep the
  ordinary image instead of guessing coverage. The game reports why it refused
  the scene source.

The format guard recognizes the current packed picture. It is not authentication
or a guarantee that a copied/frozen image is fresh. Turning off
**Steam Link native passthrough** or **Mixed Reality** takes effect when a new
ordinary image reaches the receiver; already transmitted or frozen images cannot
be recalled.
Opening VR Options keeps real-game passthrough active, and its settings apply
while the menu remains open.

## Persistence, updates and recovery

The installer owns files below `~/.local/share/SteamFrameNativePassthrough`,
`~/.config/SteamFrameNativePassthrough`, its user-service activation and temporary
per-user runtime state. It does not replace Valve binaries, flash the OS or write
the system partition/bootloader. It does not automatically restart SteamVR.

Before each activation it verifies the **Frame-side files** against the installed
package. A changed/unknown fingerprint prevents loading the receiver and restores
the owned camera setting. Temporary activation expires with the service process
or a reboot; the installed package remains available for inspection. Keeping it
installed does not mean that a new SteamVR build has been admitted. A failed or
incomplete previous launch latches activation off until you inspect `status`,
resolve the cause and explicitly use `enable`.

A **PC-side** SteamVR/Steam Link update can change the video layout or codec while
all Frame-side files remain identical. Pre-start file checks cannot detect every
such change. Native pipeline and current-image format guards retain the original
shader when their contracts do not match, but an actively packed game source can
then look like an atlas rather than an ordinary game view. There is no receiver
handshake that automatically turns off the PC source. Disable **Steam Link native
passthrough** in the game, restart the game if the ordinary image does not return,
and wait for the required compatibility update. Do the same while the receiver
is otherwise disabled or unavailable. This is conservative fallback, not a
guarantee of an invisible recovery from every update.

Stopping activation affects future processes. Running modified pipelines remain
owned by that SteamVR process until it exits. Installing while it is running
stages activation for the next normal launch. A loaded library cannot be replaced
by an update; stop SteamVR manually before updating the checkout and running
`install` again. An exact repeat preserves a manually disabled state instead of
silently re-enabling it.

Follow a disable/recovery operation with a **manual** SteamVR restart.
`emergency-off` does not need a working SteamVR camera API to remove activation;
camera-setting restoration can be separately pending and is reported by `status`.
For full removal, stop SteamVR and run `uninstall` before starting it again. An
uninstall issued while live can retain a disabled stop helper and camera receipt;
repeat it after SteamVR has stopped to finalize removal. A removed filename is
never treated as proof that an old library has unloaded.

Earlier development attempts did cause recoverable black/blue screens and Steam
startup failures. Checks and rollback are designed around that history. User-scope
files and strict refusal reduce risk; they are not proof that every future runtime
failure is impossible. Keep the project folder and a way to reach Desktop Mode
or SSH until the installation works for your configuration.
