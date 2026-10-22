# Steam Link Native Passthrough

<p align="center">
  <img src="docs/img/steam-link-icon.png" width="56" height="56" alt="Steam Link">
  &nbsp;&nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/img/steam-frame-logo-dark.svg">
    <img src="docs/img/steam-frame-logo.svg" width="240" alt="Steam Frame">
  </picture>
</p>

<p align="center">
  <img src="docs/img/flag-en.png" width="24" alt="English">&nbsp;<b>English</b>
  &nbsp;&nbsp;|&nbsp;&nbsp;
  <a href="README.de.md"><img src="docs/img/flag-de.png" width="24" alt="Deutsch">&nbsp;Deutsch</a>
</p>

> [!IMPORTANT]
> **Currently supports <img src="docs/img/windows.svg" width="18" height="18" alt=""> Windows PCs with an <img src="docs/img/nvidia.svg" width="20" height="20" alt=""> NVIDIA GPU only.**
>
> **Officially supported and successfully tested: PC SteamVR 2.18.4 (build 1792185270) · Steam Frame SteamVR 2.18.2 (build 1791331621).**
>
> These are test results, not version limits. The patch recognizes the required interfaces after updates and continues when their contracts remain intact. Untested versions show a warning; **Install anyway** enables the patch for testing.

Play supported PC VR games in your real room over Steam Link.

## Native transparency, no colour replacement

The game sends transparency separately from its colours, so no colour is removed
from the picture. The Frame combines the game with its own camera image, including
partial transparency; the camera image stays on the headset.

## Supported games

| Game | Required mod | Minimum version |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** |

Other games need an integration; installing the patch alone is not enough.

## Installation

**Enable SSH on the Steam Frame once:**

1. Open **Steam Settings → System → Enable Developer Mode**.
2. Open **Developer → Set User Password** and set a password for `steamos`
   (or run `passwd` in a Desktop Mode terminal). Keep this password for the
   installer; it is separate from your Steam account password.
3. In Desktop Mode, open a terminal and enable SSH across reboots:

   ```bash
   sudo systemctl enable --now sshd.service
   ```

   If prompted, enter that password. Password input does not show any characters.

See [Valve's device setup](https://partner.steamgames.com/doc/steamhardware/steamframe/setup) for the settings.
Leave the Frame awake on the same network as your PC and stop the VR stream.

**Run on your Windows gaming PC — PowerShell:**

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
```

The assistant finds the Frame, checks both devices and guides you through installation.
It downloads its own tools; enter the Frame password when asked and keep it awake
until setup finishes.

Start your Steam Link stream and enable native Steam Link passthrough in the game.
Run the same command again to **update, uninstall or restore the previous setup**.

## For developers

[Integrate your game](docs/DEVELOPING.md) · [Tested versions and evidence](docs/COMPATIBILITY.md) ·
[Setup and recovery reference](docs/PC-FRAME-SETUP.md)

## Credits

Based on [Jake Lurie's steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Its history and [MIT license](LICENSE) are preserved; native transparency is
developed here by McFredward and contributors.

Unofficial project, not affiliated with Valve.

## Confirmed Battles continuity (PC 0.2.17)

The October 22 follow-up headset test reports no black flashes, glitches or
unwanted recenter after scenario loading. Logs confirm 8,719 masks received,
zero rejected masks/uploads, 10,330 exact encoder outputs with zero unmatched,
and no new stream/decoder reset during gameplay. Packet retries and one late
metadata message remain, without the previous unrecoverable-loss resets.
This records the tested run, not immunity to future network loss.

PC 0.2.17 reserves metadata peaks and bounds the encoder's single-frame video
buffer. Frame 0.2.13 and Gloomhaven's v1 policy are unchanged. Installer 0.2.17
recognizes the confirmed current SteamVR versions and PC runtime fingerprint;
**neither native patch needs updating** if already installed. No Frame writes
or restart are required. [Evidence](docs/development/BATTLES-BURST-BUDGET.md#confirmed-follow-up-october-22).

## Steam Link bitrate and OVR Advanced Settings

Both **Auto and a fixed Steam Link bitrate** use the same v2 coverage budget.
The patch reserves space for mask metadata inside the encoder-requested bitrate
and bounds video bursts; it does not require Auto to lower the requested value.
At a fixed setting, video compression can increase as mask complexity grows.
A fixed 100 Mbit/s CBR software test keeps the target constant while mask sizes
change and confirms the combined video/metadata budget. The successful headset
run above used **Auto**; a fixed-bitrate headset run is not yet confirmed.
If the configured rate is too low even for mask data and the retained video
floor, this policy cannot make it fit; diagnostics report the excess without
dropping masks. No extra encoder resets or calls are injected.

**Known incompatibility: OVR Advanced Settings.** In the Demeo tests it caused
unwanted tracking-origin changes/recenters; tests with it closed no longer
showed that behavior. Close OVR Advanced Settings while using this mod and
native passthrough. Gloomhaven's separate test history does not establish that
this combination is safe for Demeo.

## Update recognition (introduced in installer/PC0.2.16, Frame0.2.13)

SteamVR 2.18.4 changed the PC sender's internal positions. The patch finds
its public imports and DirectMode interface in the current module, retaining the
actual native shader and resource bindings. The Frame recognizes its public
ELF/V4L2/OpenVR contracts; shader ID/debug metadata changes are accepted only
when normalization proves unchanged executable logic and bindings. An unknown
version or whole-file digest alone no longer disables passthrough.

PC SteamVR 2.18.4 is now officially supported following the successful Battles
headset test above. Older PC 2.17.10 tests remain recorded in the
[test history](docs/COMPATIBILITY.md). Gloomhaven's
existing producer protocol and encoder policy remain unchanged. The original0.2.16/0.2.13 update changed
the Frame component; later updates with unchanged Frame files,
settings and components still leave the Frame untouched and do not restart it.
PC SteamVR remains closed after setup. See the [implementation and validation
limits](docs/development/NATIVE-UPDATE-RECOGNITION.md).
