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
> **Steam Frame SteamVR 2.18.2 · build 1791331621**
>
> **Currently supports Windows PCs with an NVIDIA GPU only.**
>
> Experimental. The installer checks compatibility.

Play supported PC VR games in your real room over Steam Link.

## Native transparency, no colour replacement

The game sends transparency separately from its colours, so no colour is removed
from the picture. The Frame combines the game with its own camera image, including
partial transparency; the camera image stays on the headset.

## Supported games

| Game | Required mod | Minimum version |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** (upcoming) |

Other games need an integration; installing the patch alone is not enough.

## Installation

**Enable SSH on the Steam Frame once:**

1. Open **Steam Settings → System → Enable Developer Mode**.
2. Open **Developer → Set User Password** and choose a password.
3. In Desktop Mode, open a terminal and enable SSH across reboots:

   ```bash
   sudo systemctl enable --now sshd.service
   ```

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

Experimental: occasional passthrough interruptions and simultaneous game/Steam-menu
composition are still being investigated.

## For developers

[Integrate your game](docs/DEVELOPING.md) · [Compatibility](docs/COMPATIBILITY.md) ·
[Setup and recovery reference](docs/PC-FRAME-SETUP.md)

## Credits

Based on [Jake Lurie's steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Its history and [MIT license](LICENSE) are preserved; native transparency is
developed here by McFredward and contributors.

Unofficial project, not affiliated with Valve.
