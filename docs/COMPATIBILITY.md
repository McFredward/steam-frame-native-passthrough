# Compatibility and current limits

Steam Link Native Passthrough requires a supported game integration and a
compatible Steam Frame receiver. It does not add mixed reality to an unmodified
game. The Frame camera stays on the headset; its video is never streamed to the
PC.

## Supported combination

| Component | Stable Frame-only route | Paired PC + Frame preview |
|---|---|---|
| Headset | Steam Frame | Steam Frame |
| Frame software | **SteamVR 2.18.2, build 1791331621, Linux ARM64**; exact runtime checks must also pass | Same admitted Frame runtime, plus the matching receiver companion |
| PC sender | Ordinary Steam Link; no PC sender companion | Pinned Windows SteamVR sender, **NVIDIA + HEVC**, with the matching companion |
| Game integration | Independent opacity packed alongside each current eye's colour image | Matching integration supplying ordinary RGB and independently associated coverage |
| Installation | [Desktop Mode or SSH on the Frame](INSTALL.md) | [PC setup assistant](PC-FRAME-SETUP.md#start-the-assistant) |
| Current evidence | Real-game native passthrough, fractional transparency and corrected stereo edges confirmed in the headset | Focused software tests; **combined headset picture and retained RGB quality not yet confirmed** |

The supported game and minimum release are listed separately in the
[README](../README.md#supported-games). Preview testing needs a corresponding
preview build of that integration. Steam Frame **standalone** passthrough is a
separate game feature and needs neither of these streamed receivers.

A version label alone is insufficient: the exact checked runtime files,
interfaces and rendering contracts must match. The Frame version does not imply
a particular PC SteamVR version. Linux/Proton game detection is separate from
native Linux sender support; the paired preview cannot install a Windows DLL
into Linux SteamVR. Other headsets, Virtual Desktop, ALVR, HDR and unsupported
source/stereo formats are outside this validated combination.

## What has actually been shown

On 2026-10-17 the maintainer confirmed real GloomhavenVR gameplay over the Frame's
native camera using ModBuild686. Earlier synthetic tests also showed fractional
transparency. Application logs recorded 4040×4040 MultiPass/MSAA8 eyes; those
allocations do not establish the encoded stream's resolution or sharpness.

The initial visible result used a temporary receiver. The persistent public
package was subsequently installed and loaded across two SteamVR service starts,
including a native-UI startup health check. This is not certification of a full
OS reboot, all future updates, every material or end-to-end latency. See the
[real-game result](research/real-game-visible-run-20261017.md) and
[persistent-setup validation](research/persistent-user-setup-20261017.md).

The paired route is a later, separate preview. Software admission, a handshake
or a healthy SteamVR startup must not be reported as a successful headset
quality/menu test. Its intended full-colour path remains unconfirmed on hardware.

## Picture, edges and menus

**Stable route:** colour and opacity share one video image. The current v3
layout uses equal 2048² logical colour/opacity grids inside a 3168² carrier.
Packing and lossy encoding can reduce scene sharpness even when the game's eye
allocation is much larger. Corrected edges and both-eye presentation were
confirmed, but comparison with ordinary streaming still found detail loss.
[The quality investigation](research/stream-quality-comparison-20261018.md)
records that limit. Legacy v1 remains accepted; experimental v2 is quarantined
after its failed stereo test, as explained in the
[regression review](research/eye-divergence-20261018.md).

**Paired preview:** the sender/receiver companions are intended to retain the
ordinary colour image and carry coverage associated with that exact source
session, frame and eye. No separately received “latest mask” may be substituted.
This is a design and software-test property, not yet a visible resolution or
latency result. Unsupported codecs do not gain paired passthrough; normal Steam
Link use must remain available. If the game selects the legacy fallback, its
Frame-only limits still apply.

Fractional opacity is supported by the coverage model. It does not give a game
shader access to the real camera for refraction. Arbitrary glass, background-
sampling effects and every transparent material family are not certified.
The source must provide RGB contribution and opacity for the same current eye;
an alpha-capable texture alone does not establish correct coverage.

For the legacy route, the current game integration pauses native passthrough
when XR input focus is lost or unknown, then resumes only with a fresh stereo
pair. This avoids keeping its packed source active behind a known full
SteamVR dashboard. The desired setting is retained. Not every local quick-menu
variant is proven to produce that focus signal. **Simultaneous game and local
Steam menu in mixed reality remains unconfirmed**, including on the paired
preview; do not treat the focus fallback as that feature.

Opening the game's own VR Options keeps real-game passthrough active. Turning
off native passthrough or Mixed Reality affects newly submitted images;
already transmitted or frozen pictures cannot be recalled.

## Persistence and updates

Both installers use owned user-directory files and activation settings. They do
not replace Valve binaries, flash SteamOS or write the system partition or
bootloader. Installed files remain between sessions; they are not a promise of
compatibility after an update.

The Frame-only receiver checks Frame runtime fingerprints before activation.
Unknown files pause activation; an incomplete startup can latch it off until
inspected and explicitly enabled. A **PC-side** SteamVR/Steam Link change can
alter the stream without changing the Frame files. The legacy format guard is
not an authenticated handshake and cannot automatically switch off the game's
packed output. A missing or incompatible receiver may therefore expose that
packed image. Disable native Steam Link passthrough in the game and wait for a
compatible update.

The paired route additionally requires a mutually compatible session and exact
frame/eye association. A missing companion or an unsupported codec must not
activate paired coverage. This does not establish invisible recovery from all
future runtime changes, nor does it remove the separate legacy fallback's
requirements. Read the assistant's actual state instead of assuming that an
installed file means the new connection is active.

## Recovery and removal

Use the instructions for the route you installed:

- **Frame only:** [disable/recovery](INSTALL.md#recovery), then manually restart
  SteamVR. Updating or fully uninstalling a loaded receiver requires a controlled
  stop/start; the guide includes the exact steps. Installation does not schedule
  a restart or a timeout.
- **Paired preview:** [Update, Uninstall and Undo](PC-FRAME-SETUP.md#update-uninstall-and-undo)
  are managed by the assistant. Normal failed-start recovery restores the prior
  owned setup and verifies the running Frame interface. **Ctrl+C instead aborts
  without automatic Undo**, retaining recovery records for the next run.

Disabling activation does not unload code from an existing SteamVR process.
Changed/unknown files and unrelated settings are preserved. Earlier development
attempts did cause recoverable black/blue screens and startup failures; strict
checks and user-scope installation reduce risk without guaranteeing that every
future runtime failure is impossible. Keep access to Desktop Mode or SSH and
retain the reported log/recovery paths until the installation works for you.
