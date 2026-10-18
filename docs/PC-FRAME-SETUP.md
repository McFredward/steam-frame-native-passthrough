# PC and Steam Frame setup assistant

**Development status:** the assistant can check and explain a PC/Frame setup.
The new sender/receiver bridge is still a prototype, so current prototype bundles
cannot install components or create pairings. An existing public Frame
scene-atlas patch is reported separately; it is not a negotiated receiver.

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
Bash uses system Python 3.9+ and gives the wizard `/dev/tty` as stdin so piping
the bootstrap into Bash does not swallow interactive review/SSH prompts.

## Run the assistant

Extract the complete dedicated setup archive. On Windows, open
`patch-steam-link.cmd`; on Linux, run `bash patch-steam-link.sh`.
Keep its `setup` folder and supplied payloads alongside the launcher.

Windows uses a bundled, release-verified Python runtime when provided, otherwise
an installed Python 3.9+ launcher/interpreter. Missing Python produces an explicit
instruction; nothing is downloaded automatically. Both platforms need an OpenSSH
client. The native Linux sender is not yet reviewed: Linux can check/plan, but
the assistant refuses to install a Windows DLL there. A Windows game running
under Proton does not turn a Linux SteamVR process into a Windows driver host.

The assistant first inventories the PC and Frame without installing anything.
It tries a previously selected address and the `steamframe.local` /
`steam-frame.local` names, then offers an optional small private-network search
and manual address entry. The scan checks port 22 only on one private IPv4 /24
or smaller. It never scans automatically. Enable Frame SSH beforehand using the
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

Use **Uninstall** with a valid setup bundle to unregister the exact owned PC
provider and invoke the existing transactional Frame uninstaller. Changed or
unknown files are preserved. Credentials are retained, so uninstalling code
does not destroy a user's rollback key. The assistant does not delete another
driver, overwrite Valve binaries or require administrator access.

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
The assistant invokes the existing transactional native setup, never rewrites
service definitions itself, and removes only its unchanged owned staging files.

The fixture executes this actual embedded remote agent in temporary homes with
an explicitly fake native helper/runtime, and separately mocks system/SSH
tools. It validates control flow and file safety, not Windows ACL execution,
actual driver loading, Frame deployment or headset image quality. Existing
native setup tests cover that helper's own transaction policy; this focused
wizard suite is not a new full native test run.
