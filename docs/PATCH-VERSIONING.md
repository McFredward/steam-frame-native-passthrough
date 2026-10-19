# Patch versions

`VERSION` contains this project's semantic version, initially **0.2.0**. It is
independent of SteamVR's supported runtime version, GloomhavenVR's minimum mod
version, the wire protocol version, and the source revision. The product remains
experimental; a semantic version is not a compatibility or hardware guarantee.

Increment `VERSION` for **every externally published patch candidate**. Increment
the patch component for corrections and the minor component for a new feature set.
Publishing changed native artifacts under the same version is not the normal
release workflow. The installer still handles that defensively by offering
Update whenever the exact checked package differs.

Routine candidates are delivered through the stable repository installation
line in the English/German READMEs. The maintainer does not want a separate
version-tagged GitHub Release for every candidate. Existing installer download
channels remain delivery infrastructure; package and installed-component
versions remain independently inspectable.

The package assembler source-pins `VERSION` and stamps it in the outer package's
owned `VERSION` file, source/packaging receipts, and the two component entries in
`stream-bridge-bundle.json`. Both components currently share the project version;
the installed PC and Frame receipts are inspected independently, so a partial or
older installation can show different current versions.

The installer displays each component's **installed patch** and **available
patch**, and repeats the independent `from → to` values in the Apply confirmation.
Inspection JSON and bounded session logs include the same values. There are
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

The online bootstrap's strict outer schema is unchanged. `VERSION` is an
additional ordinary source-pinned member, and all new semantic metadata is in
the inner bridge manifest or owned installation receipts. The receiver handshake
and video/coverage associations do not use semantic versions as authority.
