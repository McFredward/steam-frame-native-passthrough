# Working on Steam Link Native Passthrough

- For a new game, start with `docs/DEVELOPING.md`, then
  `docs/development/GAME-INTEGRATION.md` and `INTEGRATION-LESSONS.md` in that
  directory. They describe the current ordinary-RGBA route and an agent handoff.
  Dated investigations and atlas experiments are historical evidence.
- Integrate and push only on `dev`. The maintainer's 2026-10-19 blackout-test
  instruction supersedes the previous publication authorization: do not update
  `main` until the maintainer explicitly approves another release. Keep upstream
  history and attribution; worker branches stay local.
  Use separate worktrees and disjoint file ownership.
- Commit as `McFredward <frederik@lissek.info>`. Speak German to the maintainer;
  source, comments and developer documentation are English.
- The intended input is real per-eye scene coverage, independent of scene RGB.
  Never derive mask coverage from a key colour in the explicit-mask path.
- The maintainer explicitly authorized a PC Steam Link sender companion on
  2026-10-18, superseding the earlier application/Frame-only restriction for this
  work. Require a mutually confirmed compatible sender/receiver session before
  enabling the new coverage route. Missing, incompatible or disconnected peers
  must preserve ordinary Steam Link and report a bounded useful status. A
  handshake alone never proves exact video/coverage association. This does not
  authorize an unattended Frame installation or restart; device changes still
  require the maintainer's coordinated permission and readiness.
- The maintainer's 2026-10-18 installer ruling authorizes the interactive setup,
  after its concrete Apply confirmation, to close owned PC SteamVR processes,
  atomically prepare Frame files while its UI stays running, perform the required
  immediate coordinated service restart, monitor startup and restore the original
  files/settings on failure. Do not leave Frame SteamVR stopped during file work
  or while waiting for user input. Rollback is complete only after Steam and
  SteamVR on the Frame are running and the original UI passes startup health.
  Failed discovery/operations offer an explicit retry; uncertain writes are
  reconciled through exact owned checkpoints, never blindly replayed. This
  authorizes installer behavior, not uncoordinated developer device testing.
- The maintainer's latest 2026-10-18 cancellation ruling makes Ctrl+C an
  immediate setup abort without automatic rollback. Retain pending recovery
  records; do not restore files, start another restart or retry after that
  explicit abort. This exception does not remove ordinary failed-start recovery
  or the requirement to restore a running original Frame when Undo is selected.
- A mask belongs to one source session, video frame, eye and extent. A compositor
  submit counter, approximate timestamp or latest received mask is not proof of
  that association. Missing/invalid associations never grant game composition;
  the active-MR recovery rule below determines camera-only versus ordinary output.
- The maintainer's 2026-10-22 ruling supersedes opaque recovery for an active,
  authenticated explicit-passthrough source: keep live camera-only output during
  missing/invalid game data or decoder recovery, and resume game rendering only
  with its exact current mask/image proof. Camera-only output must contain no
  game pixels or stale mask. Apply this design generally across apps, including
  older explicit-RGBA producers, through negotiated capabilities; do not fork a
  Demeo-specific route. No active source/peer/camera grant retains ordinary video.
- Preserve native reprojection, distortion and foveation mapping. Do not activate
  a replacement shader until its descriptor and frame/UV bindings are established.
- Do not redistribute Valve binaries, shader source or disassemblies. Diagnostic
  captures go into ignored `build/` or a private output directory.
- Run focused tests for bounded changes; report software evidence separately from
  a headset test. The maintainer confirmed real GloomhavenVR native passthrough
  with Build686 on 2026-10-17. That temporary-process hardware result does not
  establish persistent-installation, update safety or arbitrary-game support.
- Keep allocations, queued frames, reassembly and logging bounded. No file I/O,
  socket waits or shader compilation on a compositor rendering thread.
- The maintainer's 2026-10-19 continuity requirement makes every active-MR game
  frame's exact coverage mandatory. Routine mutex contention, snapshot bursts
  or known individual gaps must not repeatedly reset the whole paired session
  or make normal gameplay flash opaque. Fix availability/ownership rather than
  substituting stale masks or weakening unknown-loss/frame/eye proof. The
  camera-only rule above supersedes historical opaque recovery for an active
  authenticated MR source. Connection/camera-grant loss cannot borrow an old
  mask or stale camera authority.
- Keep semantic versions and per-PC/Frame installation status, but do not create
  separate version-tagged GitHub Releases for routine candidates (2026-10-19).
  Publish the stable repository installation line in both READMEs. Deliver the
  validated archive from an immutable repository commit with exact size/SHA256
  in the bootstrap; the installer does not depend on GitHub Releases. The
  maintainer explicitly requested stable release 0.2.22 on 2026-10-22. That does
  not authorize future releases or routine candidate tags without a new request.
- The maintainer's 2026-10-21 PC-only update instruction requires unchanged
  Frame components, settings and services to remain untouched. Such an update,
  its failure recovery and interrupted-run continuation must not restart the
  Frame or replay the paired Frame installation/finalization workflow.
- Installer `VERSION` and PC/Frame `PATCH_VERSIONS.json` are independent. An
  installer-only update must not invent a native patch version or recommend a
  patch update when the installed native components are already current.
- The maintainer's 2026-10-19 installer request permits installation on untested
  SteamVR versions after an explicit additional warning and confirmation. Do not
  hide Install/Update/Repair solely because a runtime differs. Installation
  confirmation enables the patch for testing, including mismatching file pins;
  never install it disabled solely because that build is untested (2026-10-21).
  Whole-file/runtime-version pins describe tested observations, not activation
  allowlists. Recognize bounded public APIs/layouts and unchanged shader logic
  on both PC and Frame after updates; refuse only when necessary properties or
  current frame/eye/source proof cannot be established. READMEs list successful
  tested versions, not hard version compatibility limits (2026-10-21).
  Keep native ABI/shader/frame correctness checks and preserve removal/recovery.
- PC SteamVR must remain closed after Apply, failure recovery and Undo. Never
  relaunch it to restore its historical running state (2026-10-21). A fresh run
  may reconcile an old stop-only journal using freshly verified Valve images;
  preserve its original receipt and reject image changes during an operation.
- Clean completed temporary test artifacts after every run (maintainer request,
  2026-10-11). Retain small validation records and only the build outputs still
  needed for review/deployment. Protect active workers, hardware logs, source
  checkouts and private reference captures; never purge `/tmp` wholesale.
