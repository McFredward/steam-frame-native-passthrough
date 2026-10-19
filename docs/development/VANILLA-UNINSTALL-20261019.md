# Vanilla uninstall and exact operation Undo

The maintainer's 2026-10-19 requirement distinguishes two different outcomes:

- **Uninstall:** ordinary Steam Link with none of our installed patch generations
  activated, and the original owned SteamVR preferences restored.
- **Undo / Restore previous setup:** the exact state before the latest installer
  operation. This can intentionally include a previously installed patch.

Do not implement full Uninstall by merely replaying a migration's predecessors.
The paired receiver suspends the older Frame-only receiver during installation;
those predecessors include the old launch hook, Vulkan layer and enable choice.
Restoring them is appropriate for Undo, but is not a vanilla uninstall.

## Evidence and device cleanup

The maintainer's first "uninstalled" comparison still loaded the old
`libVkLayer_frame_pixel_snapshot.so`, enabled by its restored 80-series service
drop-in and runtime-manifest symlink. The old camera override remained enabled.
It was not a vanilla control and could not establish that the reported green
camera also occurred without our components.

The authorized complete removal used the actual production
`FrameInstall.undo(backup=True, live=True, purge_legacy=True)` with verified helper
source, the exact current native OpenVR provider, and the old installation and
camera receipts. Fifteen exact owned paths were removed. The original camera
receipt recorded an absent `camera.forceSceneAlphaBlending` key; removal used the
native API to restore absence, not a guessed `false` or a whole settings file.

Files and settings were prepared while the existing interface remained running.
One coordinated Steam/SteamVR restart then produced fresh service/UI processes.
Verification found neither installation, no activation hook and no mapped patch
library; the original camera key was absent. The maintainer confirmed that both
the interface and its camera background were normal. The completed operation's
exact backup was retained as inert recovery evidence; active pending setup and
lifecycle records were finished without a second restart.

This establishes the clean baseline for that run. It does **not** isolate whether
the retired library, camera override, startup ordering or native camera state was
the individual cause of the preceding green-camera failure. Do not describe the
prior nominal uninstall as evidence exonerating the patch, or the clean result
as proof that a newly installed receiver has no camera regression.

## Ownership and failure behavior

`steam_link_setup_legacy` builds a pure removal plan before any mutation. The
receipt limits exact owned paths, hashes, manifest-link target, control schema
and original camera preference. Old executables are never run. The current
reviewed camera helper and a pinned native provider perform the settings delta.
Paired rollback skips these removal paths so the retired generation is never
briefly reactivated. Exact checkpoint predecessors remain available for an
explicit Undo.

An interrupted Uninstall can have already removed its installation receipt or
some payload files. Only the validated private Uninstall checkpoint may supply
their exact original bytes and planned absence. The remover reads that authority
in memory and verifies remaining hashes; it never republishes a retired receipt,
hook or library to resume. New removals keep the old receipt until last. A pending
update transfers its existing proven state and original Undo authority as metadata
before direct removal, with one final vanilla restart and picture check.

Unknown or changed files, a missing launch-hook ownership entry, an orphan camera
receipt or an unrecorded camera baseline must prevent a **complete** uninstall
claim. Preserve the evidence and explain the unresolved ownership rather than
guessing a setting or deleting another owner's file. Backup and log files are
inert and may remain; no recovery receipt itself authorizes a restart.

The focused tests exercise production camera and transaction code with the
native boundary replaced. They prove ownership, ordering, settings deltas and
checkpoint behavior, not headset pixels. The separate visible confirmation
above is hardware evidence from the maintainer.
