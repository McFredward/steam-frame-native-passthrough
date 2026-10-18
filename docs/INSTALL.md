# Stable Frame-only installation

This guide installs the **stable Frame-only receiver**, whose native passthrough
has been demonstrated in the headset. It shares video space between colour and
transparency, which can reduce sharpness. For the separate Windows/NVIDIA/HEVC
**PC + Frame preview**, use the [paired setup assistant](PC-FRAME-SETUP.md). Its
intended full-colour quality is not yet confirmed in a headset test.

Installation runs **on the Steam Frame**, either directly in Desktop Mode or
from a PC over SSH. Both methods install the same files in your home directory.
Keep the downloaded project folder for updates and recovery.

## Before you begin

You need a Steam Frame, a Windows PC with SteamVR and Steam Link, and a
[supported game integration](../README.md). Check the accepted
[Frame runtime and compatibility limits](COMPATIBILITY.md#supported-combination)
before installing. This receiver alone cannot add passthrough to an unmodified
game.

The installer builds and checks the receiver locally. It needs Bash, Python3,
CMake, `g++`, `spirv-val` and Vulkan headers; Git is needed for the clone route.
These tools were present on the checked Frame. Missing requirements are reported;
do not unlock the system partition or replace SteamVR files.

Remove an older experimental or chroma-key installation using its own rollback
procedure first. This installer refuses conflicting installations. Its first
successful install requests activation; repeating the installation preserves a
state you explicitly disabled. It never restarts SteamVR automatically.

## Directly on the Frame

1. Save your game, stop the PC VR stream and open a terminal in the Frame's
   Desktop Mode.
2. Download and install the project:

   ```bash
   git clone https://github.com/McFredward/steam-link-native-passthrough.git
   cd steam-link-native-passthrough
   bash setup.sh install
   bash setup.sh status
   ```

   Alternatively, use GitHub's **Code → Download ZIP**, extract the archive and
   open a terminal in the extracted folder. Run `bash setup.sh install`, followed
   by `bash setup.sh status`. You do not need `chmod` or a manual shader download.
3. Read the result. When you are ready, **manually restart SteamVR once** to load
   the receiver. This interrupts the Frame's VR interface and any stream:

   ```bash
   systemctl --user restart steamvr.service
   ```

If installation or compatibility checks report a problem, resolve it before
enabling the game's native passthrough option. See [recovery](#recovery).

## From a PC over SSH

Enable Developer Mode and SSH on the Frame, set its user password and find its
current network address in the Frame's settings. Valve's
[developer setup](https://partner.steamgames.com/doc/steamhardware/steamframe/setup)
describes device access; menu locations can change with SteamOS updates.

### Keep SSH enabled after a restart

In the Frame's **Desktop Mode**, open a terminal and run:

```bash
sudo systemctl enable --now sshd.service
```

Enter the user password you set in the Frame's Developer settings when prompted.
This starts SSH now and enables it for future boots; no separate autostart script
is needed. Keep Developer Mode enabled.

Check the service, including after restarting the Frame:

```bash
systemctl is-enabled sshd.service
systemctl is-active sshd.service
```

The results should be `enabled` and `active`. Use the Frame's current Wi-Fi
address when connecting; it can change after a restart.

### Connect and install

1. Save your game and stop the PC VR stream. Open PowerShell or a terminal on
   the PC and connect:

   ```text
   ssh steamos@<frame-ip>
   ```

2. Run the following commands **in that SSH session, on the Frame**:

   ```bash
   git clone https://github.com/McFredward/steam-link-native-passthrough.git
   cd steam-link-native-passthrough
   bash setup.sh install
   bash setup.sh status
   ```

3. Read the result. When you are ready, manually restart SteamVR once:

   ```bash
   systemctl --user restart steamvr.service
   ```

That restart interrupts the Frame's VR interface and any stream. No installer
command schedules a restart or an expiry timer. Keep SSH available while trying
an experimental build.

## Use with GloomhavenVR

1. Confirm that `bash setup.sh status` accepts the installed receiver, then
   start the PC VR stream and GloomhavenVR normally.
2. Open **VR Options → World → Scenery**. Choose **Mixed Reality** in the
   environment selector, then check **Steam Link native passthrough** directly
   below it. The native choice is available only on an identified Steam Frame
   connection through Steam Link.
3. Changes apply immediately, including while VR Options remains open. Opening
   the options does not turn passthrough off. Disable the native choice or
   Mixed Reality to stop it; there is no automatic timeout.

The matching game integration is required; see the [supported games and minimum
versions](../README.md#supported-games). Steam Frame standalone passthrough is
built into the mod and does not need this streamed receiver.

On a fresh GloomhavenVR installation, Mixed Reality starts off and the native
Steam Link preference starts on. An existing saved off preference is kept.
Connection detection does not check whether the matching receiver is installed
and active.

The game setting controls the current picture. Turning it off restores ordinary
output when the next game image reaches the headset; it does not uninstall the
receiver. **If the receiver is disabled or incompatible, turn the native game
option off too.** Otherwise, you may see the packed transmission image instead
of an ordinary game view.

## Disable

Run these commands from the project folder **on the Frame**, directly or over
SSH:

| Command | Purpose |
|---|---|
| `bash setup.sh status` | Show installation, compatibility and recovery state. |
| `bash setup.sh disable` | Disable activation for subsequent SteamVR starts. |
| `bash setup.sh enable` | Re-enable a checked installation after resolving a refusal. |
| `bash setup.sh uninstall` | Remove owned files once SteamVR is stopped; report any pending cleanup. |
| `bash setup.sh emergency-off` | Disable activation even when SteamVR's interface or camera API is unavailable. |

First turn off **Steam Link native passthrough** in the game. Disabling or
uninstalling the receiver cannot undo changes already loaded in a running
SteamVR process. Save your work and finish a disable/recovery operation with a
**manual SteamVR restart**:

```bash
systemctl --user restart steamvr.service
```

## Uninstall

For a complete uninstall, stop SteamVR manually, run `bash setup.sh uninstall`,
then start SteamVR normally. If you ran uninstall while SteamVR was active and
it reports pending cleanup, stop SteamVR and run the command again before
starting it. Status also reports camera-setting restoration that could not yet
complete.

## Recovery

If SteamVR does not start normally, use Desktop Mode or SSH, open the project
folder and run:

```bash
bash setup.sh emergency-off
bash setup.sh status
```

Then restart SteamVR manually. Emergency-off removes activation independently
of whether the camera API works; restoring a saved camera setting can still be
pending. It does not claim to unload a library from a running process.

A failed or incomplete previous launch can leave activation disabled. Inspect
`status`, resolve the reported problem and use `bash setup.sh enable` before
your next manual restart. Do not bypass compatibility checks.

If the game shows a packed or incorrect image, turn off **Steam Link native
passthrough** in GloomhavenVR. Restart the game if the ordinary image does not
return, inspect `status` and consult the [compatibility notes](COMPATIBILITY.md).
Opening VR Options alone does not turn the source off. The game temporarily
pauses the legacy source on known XR input-focus loss; this is not a guarantee
that every local Steam menu is composited correctly over passthrough. See
[picture and menu limits](COMPATIBILITY.md#picture-edges-and-menus).

Earlier development attempts caused recoverable black/blue screens and Steam
startup failures. User-directory installation and startup checks reduce repeat
failures; they cannot guarantee that every future SteamVR change is harmless.
Keep a way to reach Desktop Mode or SSH until the installation works for you.

## Update

The installed files remain across updates. Each SteamVR start checks the Frame's
runtime files again; an unknown fingerprint pauses activation. A PC-side
SteamVR or Steam Link update can also change the stream without changing those
Frame files. If that happens, keep the game's native option off until a
compatible update is available.

To update a Git checkout:

1. Save your work and stop SteamVR manually.
2. In the project folder on the Frame, run:

   ```bash
   git pull --ff-only
   bash setup.sh install
   bash setup.sh status
   ```

3. Inspect the result, then start SteamVR normally when ready. A loaded library
   is never overwritten in place.

If you installed from a ZIP, download and extract the updated project, then run
`bash setup.sh install` and `bash setup.sh status` from its folder while SteamVR
is stopped. Keep a copy of the project available for recovery.

### If Steam starts SteamVR again during maintenance

Steam can immediately request another start after a manual stop. If the installer
still reports that SteamVR is running, use SSH and temporarily block those start
requests before repeating your update or uninstall:

```bash
systemctl --user mask --runtime --now steamvr.service
```

This pauses the Frame's VR interface. After completing the operation, remove the
temporary block you just created and start SteamVR again:

```bash
systemctl --user unmask --runtime steamvr.service
systemctl --user start steamvr.service
```

Use this only when the service was not already masked; preserve any pre-existing
mask. `--runtime` does not change the system installation and expires at reboot.
Do not use `--force` or replace an existing unit file. If the maintenance command
fails, remove your temporary block and start SteamVR again before investigating.

See [compatibility and recovery limits](COMPATIBILITY.md) for the supported scope
and [the developer guide](DEVELOPING.md) for architecture and integration details.
