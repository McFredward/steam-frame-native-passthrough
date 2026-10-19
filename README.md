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
> **Experimental PC + Frame setup:** Windows · NVIDIA · HEVC. Exact runtime checks must pass.

Play supported PC VR games in your real room over Steam Link.

## Native transparency, no colour replacement

The game supplies transparency separately from its colours, so no colour has to
be removed from the picture. The Frame combines the game with its own camera
image, including partial transparency; the camera image stays on the headset.

**[Install or update](docs/INSTALL.md)** · **[Compatibility](docs/COMPATIBILITY.md)** ·
**[Uninstall or Undo](docs/PC-FRAME-SETUP.md#update-uninstall-and-undo)**

## Supported games

| Game | Required mod | Minimum version |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** (upcoming) |

Other games need their own integration. The patch alone cannot add mixed reality
to an unmodified game.

## Get started

Paste this into **PowerShell on your PC**:

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
```

1. Follow the [guided setup](docs/PC-FRAME-SETUP.md#start-the-assistant).
   It shows installed and available patch versions for PC and Frame and offers installation or update.
2. Start Steam Link and a supported game, then enable its native Steam Link
   passthrough option. Turn that option off to restore the ordinary picture.

The new route keeps the complete colour image for the game. **Its final picture
quality and simultaneous game/Steam-menu composition are still being validated
on hardware.** Unsupported or missing companions retain ordinary video.

Only this paired route is developed and offered for installation. The former
Frame-only receiver is [retired](docs/history/ATLAS-RECEIVER.md); old release
archives are historical downloads, not a recommended alternative.

## For developers

[Integrate your game](docs/DEVELOPING.md) ·
[Protocol and runtime limits](docs/COMPATIBILITY.md)

## Credits

Based on [Jake Lurie's steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Its history and [MIT license](LICENSE) are preserved; native transparency is
developed here by McFredward and contributors.

Unofficial project, not affiliated with Valve.
