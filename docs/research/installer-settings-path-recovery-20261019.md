# Installer camera-target recovery (0.2.6)

## Source of the blocked update

The maintainer's `setup-20261019T154326Z-9dbe504ea043e038.jsonl`
refused `bridge-restore` before the new Update began. The authored check was
`check_aa95d2e2ac6d7e9e`: the camera restore could not be confirmed. Existing
0.2.5 pending files and a previous operation checkpoint were retained.

Read-only hardware evidence shows the first OpenVR registry config directory
is `~/.config/openvr/config`; `~/.local/share/Steam/config` is only a later
historical entry. The old installer and helper config always selected the
latter. Native IVRSettings changed the real active file, while persistence
confirmation watched the unused file. Increasing its timeout cannot repair
this disagreement. Prior tests missed it because their fake native API and
installer shared the same single settings path.

## Corrected ownership and recovery

The checked current helper resolves `VR_CONFIG_PATH`, then the selected
`VR_PATHREG_OVERRIDE`/XDG registry, using only `config[0]`. Later entries are
history, never fallback candidates. Missing, linked or unsafe registry/config
data does not authorize guessed camera writes. Every RPC receives the current
manifest-verified helper, including inventory and recovery of older receipts.

An exactly recorded old Steam-directory camera target may be recovered offline
only after proving it differs from the active public registry target. An
unresolved native write-intent lease still refuses this route. The known pending
migration/checkpoint can restore only its recorded inactive-file delta; its
baseline never transfers to the active native settings file.

A target transition keeps the helper pause present continuously. Existing
leases, pending/unknown migration records and foreign pauses are preserved and
refused. A valid completed migration, or a grantless/no-write state, permits the
owned pause to move to the verified active target. Exact operation checkpoints
include pause predecessors so Undo restores the old config before its old pause
state. An abandoned pre-publication transition remains reversible. Startup
clears only the pause matching the newly published helper config.

## Presentation

Pending earlier recovery now has its own message, distinct from an unsuccessful
restoration. Install/Update/Repair confirmations and continuations describe the
actual operation; accepted completion is not described as undone. English and
German text also distinguish reported paths from files actually created.

Textual provides F3 selectable path lists and Copy/Copy-all buttons; F4 or
Ctrl+Shift+C copies the log path. Windows uses the Unicode clipboard API;
Linux confirms a local clipboard tool. Failure displays a manual selectable
fallback rather than claiming successful copy. Password contents are never
copied by these actions. Ctrl+C remains an abort without automatic rollback.

## Validation limits

Focused tests use two actual, independent temporary settings files: the fake
native API always writes the registry-selected file, irrespective of the
installer's config. They reproduce the old mismatch and exercise inactive
recovery, update continuation, pause transfer, exact Undo, changed files and
unknown write authority. Textual Pilot covers English/German narrow terminals,
clipboard failures and password isolation. Unchanged renderer/sender binary
checks are inherited from 0.2.5; no rendering-code change is part of this repair.

These are software and read-only hardware findings. No developer device write,
restart or headset acceptance was performed for this repair. A subsequent
installer run and normal-image confirmation are still required on the Frame.
Private inputs, result logs and source pins remain in the maintainer's ignored
`steamlink-sender736` evidence directory; they are not public binary payloads.
