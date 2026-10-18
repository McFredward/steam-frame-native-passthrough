# Guided check and removal

The assistant checks PC SteamVR and the Frame over SSH, and can remove an
existing installation you explicitly select. **It does not install the new
sender/receiver bridge or fix image quality.** It never restarts Steam, SteamVR
or the headset, and never creates a pairing in this maintenance mode.

On Windows, open PowerShell normally (no administrator permission needed):

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/dev/get-preview.ps1 | iex
```

On Linux, open a terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/dev/get-preview.sh | bash
```

The same command now opens a numbered **Check / Uninstall / Exit** assistant.
It does not install the unfinished full-resolution sender/receiver bridge. The
existing Frame patch can be removed after an explicit review and typed
confirmation; merely launching the command never removes or activates a patch.

The launchers pin the reviewed bootstrap by commit and SHA-256 and request the
separate `setup-manager-v1` maintenance archive. Windows includes private Python
and an SSH-client fallback; no Python packages, Windows optional features or
administrator-level installation are required. Linux reuses Python 3.9 or newer,
or automatically prepares a pinned private interpreter. Keep the Frame awake on
the same network; neither wearing it nor starting a stream is required.

The assistant automatically tries its last confirmed Frame, local Frame names,
nearby device information and one bounded local network search. It never asks
for a subnet or other network configuration. Manual IP entry is the last fallback.
The confirmed address stays in the same non-secret inspection report for reuse.
English/German follow the PC's language. The paired preview uses Textual with
keyboard/mouse navigation, visible progress and private password entry inside
the interface. Failed discovery offers another search before manual IP entry.
Passwords are reused only during that setup invocation and never written to a
log or file. OpenSSH still verifies the Frame identity; accepting a fingerprint
may update your PC's normal known-hosts file. Ctrl+C cancels waiting connections;
any already-started changes are reconciled and restored before setup exits.

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

A check can save its own non-secret PC report:

- Windows: `%LOCALAPPDATA%\steam-link-native-passthrough\setup-inspection.json`
- Linux: `${XDG_DATA_HOME:-~/.local/share}/steam-link-native-passthrough/setup-inspection.json`

Send this report when reporting setup problems. It contains no passwords,
pairing credentials or raw SSH output. An unrelated file at that location is
preserved. Add `--check` for JSON output without saving a report or prompting;
provide `--frame <IP-or-name>` to include the Frame. `--dry-run` also saves
nothing. Combining `--inspect` with an installation `--action` is refused.

## Removing an existing patch

Choose **Uninstall**, read the affected components, then type the displayed
confirmation word. The assistant uses only the patch's exact owned installation
receipt and a verified removal helper. Unknown or altered files stop removal;
other Steam files are never replaced.

If SteamVR is still running, the patch is disabled but removal can remain
pending. Stop SteamVR yourself, run the same command again and choose Uninstall
to finish restoring the original camera setting and remove the owned files. Do
not interpret a pending result as complete removal. There is no automatic
restart or timeout, and the disabled recovery helper is retained until it is no
longer needed.

Read-only tooling remains available as `--inspect --check` with an extracted
inspection profile; it never obtains removal permission. `--manage --check` also
only emits a report. The maintenance archive has a separate outer schema,
`sfnp-bootstrap-manager-v1`, readiness `maintenance`, and a
`setup-maintenance.json` profile pinning the one known removal helper. It contains
no sender/receiver activation payload. Neither the inspection nor maintenance
schema can be admitted as a complete bridge installation.

## Inspection profile

The archive contains a small `setup-inspection.json` metadata file with schema
`sfnp-setup-inspection-v1`. `expected_pc` maps `windows-x86_64` and/or
`linux-x86_64` to the fixed SteamVR vrlink driver and vrpathreg paths, each pinned
by length and SHA-256. `runtime_files` contains the known Frame reference pins
(`base`, relative `path`, `bytes`, `sha256`). There are no executable commands or
native payloads in this format. No reviewed-release admission can be obtained
from an inspection profile.
