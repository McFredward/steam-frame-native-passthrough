# Patch versions

The October22 stable release is installer **0.2.22**, PC **0.2.21**, Frame
**0.2.18**. The current repository version files remain authoritative; the
examples below illustrate independent versioning, not a runtime allowlist.

`VERSION` identifies the installer. `PATCH_VERSIONS.json` independently identifies
the PC sender and Frame receiver patches. Installer-only changes increment
`VERSION` without claiming a new native patch: installer **0.2.13**, for example,
still delivers PC patch **0.2.11** and Frame patch **0.2.12**. A semantic version never establishes
SteamVR compatibility or a hardware result.

Increment the affected component in `PATCH_VERSIONS.json` when its native patch
changes. The other component can retain its version. Publishing different native
artifacts under the same component version is not the normal workflow; exact
artifact checks still detect such differences and do not call them current.

Routine candidates are delivered through the stable repository installation
line in the English/German READMEs. The maintainer does not want a separate
version-tagged GitHub Release for every candidate. The installer payload is
stored under `distribution/` and the bootstraps use an immutable repository
commit URL with its exact byte count and SHA256. They do not require a GitHub
Release. Package and installed-component versions remain independently
inspectable. The [payload procedure](../distribution/README.md) describes the
two source-pinned publication steps.

The package assembler source-pins and ships both version files. The outer
package's `VERSION`, bundle root version and source/packaging receipts identify
the installer. The `pc.version` and `frame.version` entries in
`stream-bridge-bundle.json` come from `PATCH_VERSIONS.json`. Installed receipts
retain their component versions independently.

The installer displays each component's **installed patch** and **available
patch**, and repeats the independent `from → to` values in the Apply confirmation.
The installer version is displayed separately. A newer installer alone does not
recommend a patch update when both native patches are already current. Check is
recommended in that case; Repair remains available for missing files and setup
tools. A newer PC or Frame component offers Update. Inspection JSON and bounded
session logs include independent component values. There are
three distinct absence states: not installed (confirmed missing), unversioned
(an old installed receipt without a version), and not checked (the Frame has not
been inspected). No version is guessed from an old binary hash or a Git date.

Existing exact unversioned receipts remain readable. A confirmed update writes
the component's target version to its new receipt without replacing its initial
uninstall baseline, pairing or unrelated settings. Exact Undo restores the old
receipt bytes, including the absence of version metadata. This uses the existing
single coordinated update restart; versioning adds no restart of its own.

Version numbers never replace bundle identity, complete file hashes, ownership,
runtime admission, or checkpoint verification. A claimed matching version cannot
admit changed installed files. A known newer installed component blocks an
automatic update to an older or unversioned target. Explicit restoration of an
owned previous checkpoint remains available through Undo.

The online bootstrap's strict outer schema is unchanged. Both version files are
ordinary source-pinned members, and semantic metadata is in
the inner bridge manifest or owned installation receipts. The receiver handshake
and video/coverage associations do not use semantic versions as authority.
