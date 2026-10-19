# PC and Steam Frame setup assistant

This is the only current installation route: an **experimental paired PC and
Frame companion**, carrying transparency alongside the original colour video.
Final headset sharpness and simultaneous game/menu composition are still being
validated. The former [Frame-only atlas receiver](history/ATLAS-RECEIVER.md) is
retired; archived downloads are not an alternative installation route.

## Before you begin

- Use the supported **Frame SteamVR 2.18.2, build 1791331621**. Exact runtime
  checks must also pass.
- The installable sender preview requires **Windows, NVIDIA and HEVC**. Other
  codecs are not admitted for paired passthrough. Linux can inspect the setup
  and maintain the Frame; installing a native Linux sender is not supported yet.
  A Windows game running through Proton still uses a Linux SteamVR driver host.
- Your game needs an integration explicitly supporting this preview. See the
  [supported-games table](../README.md#supported-games); an old game-side
  integration cannot be upgraded by installing the companions alone.
- Keep the Frame awake, connected to the same network, and stop the VR stream.
  Enable [persistent SSH access](INSTALL.md#directly-on-the-frame)
  first. The assistant does not change the Frame's SSH service or system image.

## Start the assistant

On **Windows**, paste this into PowerShell:

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
```

On **Linux**, the inspection/Frame-maintenance entry is:

```bash
curl -fsSL https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.sh | bash
```

The standard entry selects only the paired experimental package; it cannot
install an old Frame-only release. The `dev` branch and `get-preview` aliases are
for development checks of this same route, not a second receiver method.
Read a downloaded script before executing it if you prefer reviewing the source.
The assistant prepares its private Python/UI dependencies and, on Windows, an
OpenSSH fallback when needed. You do not need to install Python or use `pip`.

Use **arrow keys**, **Tab**, **Enter** or the mouse. The PC and Frame cards show
what is actually installed. **Legacy patch only** identifies an old installation that can be migrated or
removed. It does not enable a retired runtime path. Technical
information and the exact log path are available in **Details**.

## Connect and apply

1. Let the assistant find the Frame. It tries known addresses and nearby
   devices before asking for an IP address. A failed search offers **Retry**,
   manual address entry or a PC-only check.
2. Confirm a new SSH fingerprint only after verifying it belongs to your Frame.
   Enter your SSH password in the masked prompt inside the assistant. It is
   reused in memory for this setup session, never written to logs or saved for
   the next run. An incorrect password can be entered again.
3. Choose the offered **Install**, **Update** or **Repair** action and review the
   concrete changes before confirming **Apply**. An unsupported runtime, missing
   payload or unknown changed file is not overwritten to force installation.
4. The assistant prepares Frame files while its current interface remains
   running, then performs the required coordinated Steam/SteamVR restart. It
   checks fresh startup and asks whether the Frame picture is normal. Keep the
   Frame awake until this is complete. **Yes** accepts the installation;
   **No** offers rechecking or an explicit Undo. A cleanup error after Yes
   must not roll back your accepted installation.
5. Start Steam Link and the game normally when setup finishes. **PC SteamVR is
   left closed after installation**; the assistant does not launch it for you.
   Enable the game's native Steam Link passthrough option and check both eyes.

The assistant may close its verified PC SteamVR processes after Apply. It does
not reboot the Frame or stop unrelated applications. A passed startup check
establishes software health, not that the game picture, resolution or menu
composition is correct.

## Update, uninstall and Undo

Run the same command again. The assistant inspects the current files and offers
appropriate actions, including **Update**, **Repair**, **Uninstall** and
**Restore previous setup** (Undo). A current installation needs no changes.
Changed or unknown files are preserved rather than silently replaced.

A normal failed install or startup triggers recovery of the previous owned
files/settings and a restart of the original Frame interface. **Previous setup
restored** means that recovery succeeded; it does not mean the new connection
was installed. An incomplete recovery remains available on the next run and
must be reconciled before another write is attempted. Retry checks its recorded
state instead of blindly repeating an uncertain operation.

**Ctrl+C aborts setup without automatic Undo or another restart.** Recovery
records are retained and their paths are shown. Run the assistant again and
choose **Restore previous setup** when ready. Undo is complete only after the
original files/settings have been checked and the Frame interface is running
again. Closing a password prompt or cancelling an action confirmation alone
does not authorize installation or uninstall.

## Logs and troubleshooting

Failures show the exact log path and any recovery-record locations. Interactive
setup logs normally live under:

- Windows: `%LOCALAPPDATA%\steam-link-native-passthrough\logs\`
- Linux: `$XDG_DATA_HOME/steam-link-native-passthrough/logs/`, or
  `~/.local/share/steam-link-native-passthrough/logs/`

Keep that log when reporting a problem. Logs contain bounded setup stages and
recognized failure reasons, not SSH passwords, pairing keys or console captures.
The PC installation receipt is `bridge-install.json` in the same application
data root. The Frame reports its exact private recovery-record path; a displayed
path alone does not prove that a record was created there.

If the streamed picture is wrong, first disable native Steam Link passthrough
in the game to return to ordinary output. If the Frame itself does not recover,
use **Restore previous setup** and retain its reported log. Do not delete unknown
files or replace Valve binaries manually. The [compatibility page](COMPATIBILITY.md)
explains runtime updates, codecs and remaining picture limits.

## Developer and automated checks

The extracted setup supports read-only JSON inspection and a proposed-change
view:

```text
python setup/steam_link_setup.py --check --frame steamframe.local
python setup/steam_link_setup.py --dry-run --action install --frame 192.168.1.20
```

These modes do not apply changes or write setup logs/credentials. Applying an
action still requires interactive review; `--action` is not unattended consent.
`--bundle` selects a complete checked bundle and `--steamvr` supplies a library
path when automatic discovery cannot find it. Native protocol, provider and
package details belong in the [developer guide](DEVELOPING.md).
