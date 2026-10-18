# Public world-quad delivery preview

The historical helper filename remains `projection-overlay-probe`, but the
current diagnostic displays **one ordinary mono world quad**, not per-eye
projection layers. It asks whether Steam Link delivers a public overlay at all.
It does not establish independent-alpha transport, preserved resolution, dynamic
stereo scene support or a production passthrough solution.

The game must keep its ordinary scene visible. `SFNP_PROJECTION_READY_V2` means
only that local API setup succeeded; it must never automatically blank the game.
A deliberately separate experiment would be needed to establish a controlled
zero-color/zero-coverage base for independent-alpha conclusions.

## Build706 finding and bounded correction

Build706 logged plausible `IVRSystem_026`/`IVROverlay_028` setup and successful
local readiness, while the user saw only the camera. Its normalized full-eye
crop `[0.4,0.6]` was not centered on the physical forward axis. From the actual
asymmetric frusta, forward tangent0 mapped to approximately `(0.896,1.011)` in
the left cropped image and `(0.092,0.958)` in the right. Both missed visible
pattern interiors. That is a concrete placement error, not evidence that Steam
Link did or did not transport the overlays. No capture established the latter.

The replacement is one square at `headPose * translate(0,0,-1.2m)`, width0.6m,
with a 2048-square mono RGBA8 image and texel aspect1. Both eyes view the same
physical plane and image; stereo/SBS flags are explicitly off. Its center has
an opaque neutral patch, surrounded by the original fractional black/gray/color
lanes and 1/2/4/8/16/32/64-texel detail bands. Fine stripes may naturally minify;
the coarse bands allow comparison without treating that as transport blur.
No scene FOV, resolution, Steam setting or receiver is changed.

The single immutable texture and CPU pixels remain owned until cleanup. One
producer `ID3D11DeviceContext::Flush` follows `CreateTexture2D` and precedes
`SetOverlayTexture`. There is no swapchain/Present to flush this sleeping helper.
[Microsoft's Flush contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush)
explains queued commands, submission before idle CPU waits, and that Flush is
asynchronous. This closes a producer-dispatch gap; it does not prove GPU completion
or that missing Flush caused the observed failure. There is no GPU wait loop.

Local width, texel aspect, texture extent, visibility and dashboard state are
reported separately. Visibility is context, not an admission gate or receiver
proof. The dashboard need not be closed to prepare the overlay; the overlay is
hidden there by its ordinary `VisibleInDashboard=false` flag.
[Valve's public header](https://github.com/ValveSoftware/openvr/blob/v2.15.6/headers/openvr.h)
documents these APIs, square-texel aspect, width-derived geometry and flags.
No geometry fit uses `GetTransformForOverlayCoordinates`: the open
[Valve issue1562](https://github.com/ValveSoftware/openvr/issues/1562) reports that
getter ignoring texel aspect/bounds/curvature. No new intersection guard replaces it.

## Build without starting SteamVR

```bash
bash pc/projection-overlay-probe-build.sh /path/to/reviewed/public/headers /private/output
```

Requires `g++`, `x86_64-w64-mingw32-g++` and reviewed public `openvr_capi.h` SHA256
`3d32d8eb5bcbe2e250610123aee8142c68f0e7d4da823f87513b6a2a333078d7`.
The [official SDK](https://github.com/ValveSoftware/openvr/tree/master/headers)
and its [license](https://github.com/ValveSoftware/openvr/blob/master/LICENSE)
apply independently. No Valve binary/SDK is downloaded or redistributed.
The build executes portable production-core/fake-API checks and cross-compiles
Win64 with warnings as errors; it does not execute Windows/D3D/OpenVR/Steam Link.

## Explicit child lifecycle

The matching in-game diagnostic must be explicitly selected. Its child interface:

```text
projection-overlay-probe.exe --stdin-pipe --openvr <absolute installed openvr_api.dll>
```

A real stdin pipe is required before OpenVR loading. EOF, any byte or a broken
pipe removes the owned overlay. `PeekNamedPipe` avoids a blocked reader/thread.
An explicitly interactive invocation uses `--probe` instead of `--stdin-pipe`;
Enter/Escape/Ctrl+C stops it. A 25ms poll is a cadence, not a timer: there is no
automatic expiry, install, restart, receiver action or persistent rollback.
The pose stays frozen at launch; keep the head reasonably still for comparison.

The helper uses public `VRApplication_Background`, which cannot start or keep
SteamVR alive. There is no `vrserver.exe` process-name gate or Overlay/Scene
fallback. It accepts a canonical absolute installed PE64 `openvr_api.dll`, without
reparse-point components. It keeps the file open read-only and restricts loader
search to its directory and System32. Proton's absolute drive mapping is permitted;
actual Proton execution remains unverified.

The single overlay is prepared at alpha0, uploaded, flushed, handed off, shown,
then revealed. Only after local state queries succeed does stdout emit exactly:

```text
SFNP_PROJECTION_READY_V2
```

`META`, `GEOMETRY` and `LOCAL` lines are bounded trusted numeric context. No native
free-form errors, device strings or serials are emitted. Every ASCII line is at
most1024 bytes; total stdout is at most8192 bytes. No periodic logs are produced.
On clean end, hide/clear/destroy precede SDK/D3D/DLL cleanup and the exact line:

```text
SFNP_PROJECTION_STOPPED_V2
```

Startup failure rolls back every owned resource and reports bounded
`SFNP_PROJECTION_REFUSED_V2 stage=<trusted-token> code=<number>` context without
ready. Failed cleanup never reports a clean stopped token. Even when ready was
emitted, missing headset output remains an unresolved delivery result; it must
not be presented as a working alpha transport or a reason to hide the game.
