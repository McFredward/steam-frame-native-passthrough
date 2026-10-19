# Install Steam Link Native Passthrough

[Deutsch](INSTALL.de.md)

Install the **experimental paired PC + Frame setup**. Both companions are
required; a Frame-only installation is no longer offered. Current support is
**Windows + NVIDIA + HEVC**, with Frame **SteamVR 2.18.2, build 1791331621** and
matching runtime fingerprints. See [compatibility](COMPATIBILITY.md).

## Directly on the Frame

Use Desktop Mode to prepare SSH access, then run the guided assistant on your
Windows gaming PC. The assistant installs both sides; running a receiver-only
installer on the Frame cannot replace the PC companion.

1. Enable Developer Mode and SSH in the Frame's settings and set the user password.
2. In Desktop Mode, open a terminal and enable SSH across reboots:

   ```bash
   sudo systemctl enable --now sshd.service
   ```

3. Check both results, including after a reboot:

   ```bash
   systemctl is-enabled sshd.service
   systemctl is-active sshd.service
   ```

   They should read `enabled` and `active`. Keep Developer Mode enabled.
4. Leave the Frame awake on the same network as the PC and stop the VR stream.
5. On your PC, open PowerShell and run:

   ```powershell
   irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
   ```

   Follow the connection and Apply prompts. The assistant handles Python and
   its UI dependencies. You do not need to install those tools yourself.

## From a PC over SSH

The same assistant discovers the Frame and connects over SSH; a manual IP is
needed only if discovery fails. You enter the password inside the assistant.
It is retained only for that setup session.

If SSH is not enabled yet, use the Desktop Mode steps above. To verify access
manually, run `ssh steamos@<frame-ip>` using the current Wi-Fi address shown on
the Frame. Valve's [developer setup](https://partner.steamgames.com/doc/steamhardware/steamframe/setup)
provides additional device-access guidance.

Run the PowerShell command above on the Windows gaming PC and choose Install,
Update or Repair after inspection. Keep the Frame awake through the coordinated
restart and picture check. The assistant keeps its current interface running
while staging files and checks that it starts again afterward. It leaves PC
SteamVR closed; start your stream normally when setup finishes.

Linux can inspect the setup and perform supported Frame maintenance:

```bash
curl -fsSL https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.sh | bash
```

A native Linux PC sender is not yet supported. Proton does not turn Linux
SteamVR into the Windows driver host.

## First run

Start Steam Link and a [supported game](../README.md#supported-games). Enable
native Steam Link passthrough in the game. Both current companions and a matching
game integration are needed. The picture should remain ordinary if they cannot
establish permission; disabling the option restores the ordinary game picture.
No feature timeout is used.

## Update

Run the same command again. The assistant checks the current installation before
offering Update, Repair, Uninstall or Undo. Unknown/changed files are preserved.
SteamVR/Steam Link updates can make an exact runtime pin incompatible; that
refuses native passthrough rather than certifying an unknown runtime. Update the
companion when a compatible release becomes available.

For [Uninstall, Undo, logs and recovery](PC-FRAME-SETUP.md#update-uninstall-and-undo),
use the same assistant. **Uninstall** removes our current and recorded older
patches; **Undo** restores the setup before the latest operation. Ctrl+C stops
setup without an automatic rollback; keep
the displayed recovery paths for the next run. Undo is complete only after the
previous owned state and running Frame interface have been verified.

The retired receiver's old archives are [historical](history/ATLAS-RECEIVER.md).
Do not use their install or enable commands with the current version.
