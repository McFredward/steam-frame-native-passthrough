# PC and Steam Frame setup assistant

## Current paired bridge preview

The explicit `dev/get-preview.ps1` entry now selects the installable
`stream-bridge-preview-v1` candidate. It requires the pinned Windows/NVIDIA
sender and Frame runtime, and GloomhavenVR Build708 or newer. The stable entry
points and historical bundle contract below remain separate. A software-validated
preview is not a verified combined headset result.

The bridge wizard uses arrow-key menus (Up/Down, Home/End, Enter and Escape) on
Windows/POSIX terminals. Redirected or unsupported terminals retain a plain
numbered fallback. Failures and missing devices offer Retry; an uncertain write
is reconciled through its owned checkpoint rather than blindly repeated.

The bridge wizard keeps one authenticated OpenSSH connection for its lifetime.
Check, install, health checks and explicit Undo reuse that connection on Windows
and Linux. OpenSSH alone owns password/fingerprint prompts; no password is
stored and no key is added. A broken connection can be reopened for a deliberate
retry or recovery; it never silently repeats an uncertain installation request.
Failed discovery candidates and the final wizard exit close their processes.

Interactive bridge runs create a bounded private diagnostic file before the
package check, outside the temporary bootstrap directory. Failures print its
exact path and the recovery-record locations before offering Undo. Windows
logs are under `%LOCALAPPDATA%/steam-link-native-passthrough/logs/`; Linux uses
`$XDG_DATA_HOME/steam-link-native-passthrough/logs/` or the usual
`~/.local/share` fallback. The PC receipt is `bridge-install.json` in that data
root. The Frame reports its exact `SteamLinkNativePassthrough/stream-bridge/setup-restore.json`
location (normally under `/home/steamos/.local/share`). A path is a recovery
location, not proof that a failed operation created a receipt there.

Logs contain fixed stages, commands, authored-check identifiers and OS error
numbers, never console transcripts, SSH prompts, payloads or pairing keys.
An authored check ID is the first16 lowercase hex digits of SHA256 over the
exact source check message, prefixed `check_`; the client displays only its own
matching static message. This preserves the reason for a remote refusal without
forwarding arbitrary remote exception text. Logging failure reports the attempted
path and never replaces the operation error. `--check` and `--dry-run` retain
their no-log-write contract.

After the concrete Apply confirmation the assistant closes only verified owned
PC SteamVR processes and starts the prepared PC runtime itself. It never stops
Frame SteamVR for file preparation: exact replacements are staged atomically
with the original mapped files and UI still alive. It then requests an immediate
coordinated restart of Steam and SteamVR and checks new service generations,
native compositor/browser startup and the user's picture verdict. SteamVR is not
restarted twice if restarting Steam already refreshed it. No device reboot or
unrelated process kill is part of this workflow.

Failed installation/startup automatically restores the current operation's exact
private `setup-restore.json` checkpoints, then restarts and checks the original
Frame interface. These checkpoints differ from installation uninstall receipts:
rolling back a repair preserves the previously installed connection. Settings
restore only owned keys and retain unrelated changes. Durable `lifecycle.json`
records retain the PC's original running state and the Frame restart baseline.
Recovery is never marked complete until original files/settings are verified,
both Frame services are running and native startup health passes. Unknown changed
files remain untouched. An interrupted/incomplete recovery offers Retry and stays
available as a menu action on the next run, even if new installation is unavailable.

The remainder describes the separate older stable/inspection bundle contracts.

**Development status:** the assistant can check and explain a PC/Frame setup.
The new sender/receiver bridge is still a prototype, so current prototype bundles
cannot install components or create pairings. An existing public Frame
scene-atlas patch is reported separately; it is not a negotiated receiver.

The [guided maintenance command](SETUP-INSPECTION.md) is available now for
**Check / Uninstall / Exit**, including removal of the old public Frame patch.
That separate maintenance archive cannot install or activate an unfinished
bridge. Stop SteamVR manually and repeat removal if it reports pending.

## Online entry points

The repository contains `get.ps1` and `get.sh` for the planned public one-line
setup. The intended stable entry points are:

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
```

```bash
curl -fsSL https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.sh | bash
```

**These are not a published complete PC/Frame installation yet.** Publish the
stable entry points and dedicated setup asset only after both native providers
and their runtime integration are admitted. Until that asset exists, the
bootstrap refuses installation instead of using an older Frame-only release.
Do not present the development snapshot as an installable passthrough fix.

The bootstrap selects only the fixed `steam-link-native-passthrough-setup.zip`
asset of this repository's latest stable GitHub release. It verifies GitHub's
reported SHA-256 digest, bounded archive paths and every inner file before
starting the assistant. These checks protect download/package consistency;
the repository and GitHub release remain the trust source, not independent
cryptographic publishers. Neither bootstrap requests elevation or a restart.
Its temporary download/runtime directory is removed after the assistant exits.

Windows setup assets must contain the pinned portable Python runtime, so users
do not need a separate Python installation for the online command. The bundle's
bootstrap manifest is `sfnp-bootstrap-package-v1` with exact `schema`,
`readiness: release`, `windows_python: setup/runtime/python/python.exe` and
`files` fields. `files` pins every archive file except the manifest itself by
length and SHA-256. The pinned `steam-link-bundle.json` and assistant engine are
mandatory; the inner bundle must independently be a complete admitted release.
Bash uses Python3.9+ (existing or privately prepared) and gives the wizard `/dev/tty` as stdin so piping
the bootstrap into Bash does not swallow interactive review/SSH prompts.

Release engineering uses `tools/package_stream_link_setup.py` to assemble the
already admitted native bundle, these launchers/engine and an explicitly pinned
official amd64 embeddable Python archive. It performs no download or installation,
retains Python's license/provenance and invokes the independent real Frame
package verifier. It refuses to relabel a prototype or an unknown native
package variant as an installable setup. Upload the resulting ZIP under the
exact asset name above; GitHub's reported asset digest is required by bootstrap.

## Run the assistant

Extract the complete dedicated setup archive. On Windows, open
`patch-steam-link.cmd`; on Linux, run `bash patch-steam-link.sh`.
Keep its `setup` folder and supplied payloads alongside the launcher.

The online Windows command prepares a bundled, release-verified Python runtime
and portable OpenSSH fallback without installing either system-wide. The Bash
command uses Python3.9+ or downloads an exact pinned private interpreter if
needed; Linux currently uses the OS OpenSSH client. The source-checkout launchers
can also reuse an existing interpreter. The native Linux sender is not yet reviewed: Linux can check/plan, but
the assistant refuses to install a Windows DLL there. A Windows game running
under Proton does not turn a Linux SteamVR process into a Windows driver host.

The assistant first inventories the PC and Frame without installing anything.
It automatically tries a confirmed previous address, local Frame names, nearby
devices and one bounded private-network search. Only if these fail does it ask
for an IP address; users never need to specify a subnet. The search checks port22
on at most one proven physical private IPv4 /24 and validates actual Frame
inventory before accepting a device. Enable Frame SSH beforehand using the
[existing Frame instructions](INSTALL.md); the assistant does not change its
SSH service or system image.

Normal OpenSSH handles keys, password prompts and fingerprint confirmation.
Verify the Frame's fingerprint before accepting a new host. The assistant never
stores an SSH password, disables fingerprint checks or sends a password through
its command line. Read-only automated checks require an already trusted host
and usable SSH key; they do not accept host keys or update SSH connection caches.

After the check, choose **Install**, **Update**, **Repair**, **Uninstall** or
**Quit** and review the concrete action. Missing, altered, unsupported or
prototype payloads refuse installation and pairing. No component is fetched as
an unreviewed substitute. Repeating an already current installation is a no-op.
“Current” includes rehashing the actual installed Frame artifacts against the
checked bundle, including the native library and setup helper. An unchanged
receipt or component marker alone cannot hide a missing or corrupted file.

All writes finish before the final exit prompt. The assistant never stops or
restarts Steam, SteamVR, the stream or the Frame. Close SteamVR before updating
loaded provider code, then start it yourself after setup. A safe refusal is not
proof that streamed mixed reality works.

## Read-only checks and plans

```text
python setup/steam_link_setup.py --check --frame steamframe.local
python setup/steam_link_setup.py --dry-run --action install --frame 192.168.1.20
```

`--check` emits JSON without writing setup logs, credentials or cached addresses.
`--dry-run` shows the proposal without staging or applying it. Applying changes
requires interactive review; supplying `--action` alone is not unattended
permission. `--bundle` selects an extracted complete setup bundle and
`--steamvr` can identify a SteamVR library that was not discovered automatically.

## Pairing, logs and recovery

A future admitted release pairs the two components using a fresh 256-bit random
key delivered through verified SSH standard input. The key never enters the
bundle manifest, process arguments or setup log. Credentials are private files:
POSIX mode `0600`; Windows uses a protected owner/system-only DACL before secret
bytes are written. Only the explicitly selected `active.json` pairing is used;
older key files remain available for explicit rollback. No automatic key search
or attempt with every old key is allowed.

After an applied action, the non-secret log is `setup-last.log` under the user's
`steam-link-native-passthrough` data directory: Local AppData on Windows, or
XDG data / `~/.local/share` on Linux. The same directory contains the checked
PC installation receipt. Frame credentials are under
`~/.config/steam-link-native-passthrough/pairings/`.
Failures after release admission also retain a redacted report describing the
possible partial installation. Unknown existing log/address-cache files are
preserved rather than adopted. Read-only and prototype refusals create no log.

Use **Uninstall** with a valid setup bundle to unregister the exact owned PC
provider and invoke the existing transactional Frame uninstaller. Changed or
unknown files are preserved. Credentials are retained, so uninstalling code
does not destroy a user's rollback key. The assistant does not delete another
driver, overwrite Valve binaries or require administrator access.
An updated Frame runtime blocks new activation but does not by itself block
ownership-based removal with a checked setup bundle. Removal uses the verified
setup helper and valid owned receipt; it does not import installed scripts or
re-admit the changed runtime as compatible. If the native uninstaller reports
pending restoration, use the checked Frame setup's **emergency-off**, stop
SteamVR manually and retry. The assistant never reports full removal while that
step is pending.

SSH cannot make two machines one atomic transaction. On failure the assistant
undoes its new pairing selection/key when ownership still matches. A Frame
package already installed successfully by the native transactional helper can
remain installed if a later PC step fails. This is a partial installation, not
a claim that the old Frame version was restored. Run **Check**, then **Repair**
to complete the missing delta. If a pairing transaction is interrupted, preserve
its private journal and use the same checked bundle for repair; never delete
unrecognized state to make a warning disappear. SteamVR may ask for a manual
stop before its native uninstaller can finish camera restoration.

## Bundle contract for release engineering

`steam-link-bundle.json` is `sfnp-steam-link-setup-v1`, with exact top-level
`schema`, `readiness`, `bridge_protocol`, `platforms` and `frame` fields.
`readiness` is `prototype` or `release`; only a complete reviewed release can
apply. `bridge_protocol` contains exact `major`, `minor` and `wire_sha256` pins.
Each component has the same protocol, its explicit `producer`/`receiver` role,
`x86_64`/`aarch64` architecture, `reviewed` and `native_bridge` booleans and a
bounded relative-file inventory of lengths and SHA-256 hashes. These manifest
fields are not cryptographic attestation or a replacement for release review.

The Windows entry uses `provider_root: pc-provider`,
`provider_manifest: pc-provider/driver.vrdrivermanifest`,
`provider_registration: user`. Its driver name is
`steam_link_native_passthrough`. `vrpathreg` pins the bytes/hash and fixed path
`bin/win64/vrpathreg.exe` inside the detected SteamVR installation. Registration
uses Valve's [documented user driver registration utility](https://github.com/ValveSoftware/openvr/wiki/Local-Driver-Registration),
including collision checks and registry readback. No arbitrary manifest command
is executed.

The Frame entry uses `package_root: frame-package`, fixed
`setup_entry: frame-package/tools/native_passthrough_setup.py`, and exact
`runtime_files` pins (`base`, relative `path`, `bytes`, `sha256`) matching the
package compatibility profile. An admitted release additionally needs its
pinned `bridge-component.json`: schema `sfnp-bridge-component-v1`, role
`receiver`, matching protocol and provider `vrlink-complete-au-v1`. An old v3
package cannot substitute for this native provider; the unchanged real native
package verifier also must admit the complete package variant.

All local payload hashes and remote runtime capabilities are checked before
writes. After explicit Apply, exact checked bytes may be staged privately on the
Frame (`0700` directories, `0600` files). The existing `verify_package` then
checks that real staged package before activation, PC registration or pairing.
Uninstallation is different: a checked helper removes only receipt-owned files;
it must not require the updated runtime to pass installation admission again.
The assistant invokes the existing transactional native setup, never rewrites
service definitions itself, and removes only its unchanged owned staging files.

The fixture executes this actual embedded remote agent in temporary homes with
an explicitly fake native helper/runtime, and separately mocks system/SSH
tools. It validates control flow and file safety, not Windows ACL execution,
actual driver loading, Frame deployment or headset image quality. Existing
native setup tests cover that helper's own transaction policy; this focused
wizard suite is not a new full native test run.
