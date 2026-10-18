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
request only the separate `setup-inspection-v2` prerelease archive. Windows
includes private Python and an SSH-client fallback; no Python packages, Windows
optional features or administrator-level installation are required. Linux reuses
Python 3.9 or newer, or automatically prepares a pinned private interpreter.
Keep the Frame awake on the same network; neither wearing it nor starting a
stream is required.

The assistant automatically tries its last confirmed Frame, local Frame names,
nearby device information and one bounded local network search. It never asks
for a subnet or other network configuration. Manual IP entry is the last fallback.
The confirmed address stays in the same non-secret inspection report for reuse.
English/German follow the PC's language. An ASCII header and terminal-only
progress animation show the steps; animation pauses before input and SSH prompts.
OpenSSH handles the usual fingerprint
confirmation and password prompt; no password is saved. Accepting a new SSH
fingerprint may update your PC's normal OpenSSH known-hosts file.

The report identifies matching, different, missing or unreadable reference PC
files. A Frame runtime match only identifies the checked reference revision; it
is not proof that the new bridge is installed or working. Existing legacy Frame
patches are shown separately.

Each PC file also has a fixed `reason` code. A successful comparison is
`reference_match`; changed bytes are `hash_or_size_mismatch`. Failed reads
identify `missing`, `permission_denied`, `sharing_violation`, `redirected_path`,
`not_regular`, `size_limit`, `invalid_metadata`, `metadata_changed` or `io_error`.
`steamvr_not_found` means no SteamVR directory was available. No raw OS error
text is copied into the report.

Public executable files may have multiple hardlinks; inspection now accepts
these only after a bounded, unchanged file snapshot. The report includes
`bytes` and `hardlinked` when that snapshot succeeds. Private credentials,
installation writes and activation checks retain their stricter existing rules.
An older report containing only `unreadable` does not establish which failure
occurred. In particular, a hardlink rejection is a source-proven possibility,
not a confirmed explanation of that user's executable.

The actual Windows embeddable-runtime fixture additionally reproduced the
executable-extension mode mismatch: path stat reports mode33279 for an EXE,
whereas fstat reports33206 for that same unchanged open file. CPython's
[`update_st_mode_from_path`](https://github.com/python/cpython/blob/v3.14.8/Modules/posixmodule.c)
adds execute bits from the filename extension. The cross-API identity comparison
now normalizes only those Windows synthetic execute bits; the before/after
comparisons within each API retain all mode and timestamp checks. This is a
concrete EXE false rejection, separate from the potential hardlink restriction.

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
