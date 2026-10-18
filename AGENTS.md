# Working on Steam Link Native Passthrough

- Integrate on `dev`; publish the validated final development state to `main`
  when requested. The maintainer explicitly authorized that publication on
  2026-10-17. Keep upstream history and attribution; worker branches stay local.
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
- A mask belongs to one source session, video frame, eye and extent. A compositor
  submit counter, approximate timestamp or latest received mask is not proof of
  that association. Missing/invalid associations must retain opaque rendering.
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
- Clean completed temporary test artifacts after every run (maintainer request,
  2026-10-11). Retain small validation records and only the build outputs still
  needed for review/deployment. Protect active workers, hardware logs, source
  checkouts and private reference captures; never purge `/tmp` wholesale.
