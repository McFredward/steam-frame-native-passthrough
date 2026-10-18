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
> **Supported SteamVR on the Frame: 2.18.2 · build 1791331621**

**For Steam Frame.** Play supported PC VR games in your real room over Steam Link.

## Native passthrough — no colour replacement

The game sends a separate description of what is see-through, so every game
colour stays available. The Frame combines it with its own live camera image,
including partial transparency; the camera picture never needs to reach the PC.

**[Install the stable Frame receiver](docs/INSTALL.md)** ·
**[Try the paired PC + Frame preview](docs/PC-FRAME-SETUP.md#start-the-assistant)** ·
[Compatibility](docs/COMPATIBILITY.md)

## Choose your setup

| Route | What to expect | Install |
|---|---|---|
| **Stable: Frame only** | Native passthrough has been confirmed in the headset. Sharing video space with transparency can reduce sharpness. | [Desktop Mode](docs/INSTALL.md#directly-on-the-frame) or [SSH](docs/INSTALL.md#from-a-pc-over-ssh) |
| **Preview: PC + Frame** | Windows + NVIDIA + HEVC. Intended to retain normal RGB resolution; **not yet confirmed in a combined headset test**. | [One-command setup assistant](docs/PC-FRAME-SETUP.md#start-the-assistant) |

The preview is a separate opt-in. It is not yet a verified fix for sharpness or
Steam-menu behaviour. See [current limits and updates](docs/COMPATIBILITY.md).

## Supported games

| Game | Required mod | Minimum version |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** (upcoming) |

Other games need their own integration. Installing this patch alone does not
add mixed reality to an unmodified game. Preview tests need a matching preview
build of the game integration.

## Get started

1. Choose one installation route above. Keep the Frame awake and stop your VR stream.
2. Start the game normally and enable its native Steam Link passthrough option.
3. Turn that option off whenever you want the ordinary picture back.

Installation remains between sessions. For **Update**, **Uninstall** or **Undo**,
use the [paired assistant](docs/PC-FRAME-SETUP.md#update-uninstall-and-undo), or the
[stable Frame instructions](docs/INSTALL.md#update) if you installed only that receiver.

## For developers

[Integrate your game](docs/DEVELOPING.md) ·
[Runtime compatibility and limits](docs/COMPATIBILITY.md)

## Credits

Based on [Jake Lurie's steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Its history and [MIT license](LICENSE) are preserved; the native-opacity path is
developed here by McFredward and contributors.

Unofficial project, not affiliated with Valve.
