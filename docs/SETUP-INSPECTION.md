# Guided setup inspection

This preview checks the installed PC SteamVR files and connects to the Frame by
SSH. **It does not install the new sender/receiver bridge or fix image quality.**
It never pairs devices, stages payloads, changes Frame files, registers a driver,
or restarts Steam, SteamVR or the headset. Ordinary Steam Link stays unchanged.

On Windows, open PowerShell normally (no administrator permission needed):

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/dev/get-preview.ps1 | iex
```

On Linux, open a terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/dev/get-preview.sh | bash
```

These preview launchers pin the reviewed bootstrap by commit and SHA-256 and
request only the separate `setup-inspection-v1` prerelease archive. Windows
uses the included official portable Python; no system Python installation is
needed. Linux requires Python 3.9 or newer, curl and sha256sum. Keep the Frame
awake on the same network; neither wearing it nor starting a stream is required.

The assistant
tries a previously provided address and common local Frame names. If they are
unavailable, enter the Frame's IP address. Searching one private subnet is
optional and happens only after you agree. OpenSSH handles the usual fingerprint
confirmation and password prompt; no password is saved. Accepting a new SSH
fingerprint may update your PC's normal OpenSSH known-hosts file.

The report identifies matching, different, missing or unreadable reference PC
files. A Frame runtime match only identifies the checked reference revision; it
is not proof that the new bridge is installed or working. Existing legacy Frame
patches are shown separately.

Inspection can save only its own non-secret PC report:

- Windows: `%LOCALAPPDATA%\steam-link-native-passthrough\setup-inspection.json`
- Linux: `${XDG_DATA_HOME:-~/.local/share}/steam-link-native-passthrough/setup-inspection.json`

Send this report when reporting setup problems. It contains no passwords,
pairing credentials or raw SSH output. An unrelated file at that location is
preserved. Add `--check` for JSON output without saving a report or prompting;
provide `--frame <IP-or-name>` to include the Frame. `--dry-run` also saves
nothing. Combining `--inspect` with an installation `--action` is refused.

## Inspection profile

The archive contains a small `setup-inspection.json` metadata file with schema
`sfnp-setup-inspection-v1`. `expected_pc` maps `windows-x86_64` and/or
`linux-x86_64` to the fixed SteamVR vrlink driver and vrpathreg paths, each pinned
by length and SHA-256. `runtime_files` contains the known Frame reference pins
(`base`, relative `path`, `bytes`, `sha256`). There are no executable commands or
native payloads in this format. No reviewed-release admission can be obtained
from an inspection profile.
