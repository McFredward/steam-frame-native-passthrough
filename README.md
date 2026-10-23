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
> Untested SteamVR versions can be tried after a warning with **Install anyway**.

Play supported PC VR games in your real room over Steam Link.

## Native transparency, no colour replacement

The game sends transparency separately from its colours, so no colour is removed
from the picture. The Frame combines the game with its own camera image, including
partial transparency; the camera image stays on the headset.

## Supported games

| Game | Required mod | Minimum version |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** |
| Demeo | [Demeo Mixed Reality](https://github.com/McFredward/Demeo_GreenScreen) | **2.0.0** |
| Demeo Battles | [Demeo Mixed Reality](https://github.com/McFredward/Demeo_GreenScreen) | **2.0.0** |
| Demeo Battlemarked | [Demeo Mixed Reality](https://github.com/McFredward/Demeo_GreenScreen) | **2.0.0** |

Other games need an integration; installing the patch alone is not enough.

Steam Link can use **Auto or a fixed bitrate**.
**Close OVR Advanced Settings when playing Demeo**; it can cause unwanted recentering.

## Successfully tested SteamVR versions

| Component | Successfully tested versions |
|---|---|
| PC SteamVR | **2.18.4**, 2.17.10 |
| Steam Frame SteamVR | **2.18.2** |

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

## Developers

To integrate another game, start with the [integration guide](docs/development/GAME-INTEGRATION.md)
and [lessons learned](docs/development/INTEGRATION-LESSONS.md).
See the [developer documentation](docs/DEVELOPING.md) for architecture, protocols and build/test instructions.

## Credits

Based on [spyjacob's steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Its history and [MIT license](LICENSE) are preserved; native transparency is
developed here by McFredward and contributors.

Unofficial project, not affiliated with Valve.
