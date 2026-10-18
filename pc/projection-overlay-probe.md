# Public projection-overlay alpha witness V2

This explicit, static PC diagnostic asks whether Steam Link preserves a public
per-eye projection overlay's **independent alpha**, rather than flattening its
RGB into the ordinary scene video. It is not a passthrough implementation,
full-resolution fix or proof that an arbitrary game can use this path.
It changes no Steam, resolution, FOV or Frame setting and installs nothing.

The matching game diagnostic must supply RGB=0 **and** coverage=0 in its v3 RGB
region only after the exact ready token below. Ordinary opaque black is not a
transparent base. Guard/mask layout stays unchanged. With that base, the helper's
INNER black lanes all contribute RGB=0 while alpha is 0, 64, 128 or 255. A black
lane hiding the camera, next to the transparent lane showing it, is the positive
witness. Merely seeing colored rectangles, fractional color or a local PC overlay
is not proof: flattened RGB could produce those results without independent alpha.
The host must also rule out any other independently delivered UI/background.

## Scope and geometry

- Reviewed public `IVRSystem_026` and `IVROverlay_028` only, initialized as
  `VRApplication_Background`. This cannot start or keep SteamVR alive. There is
  no `vrserver.exe` process-name gate, Overlay/Scene fallback or autostart.
  If Background cannot create/configure the overlays, the test refuses.
- Two immutable **2048 x 2048 RGBA8** textures are created once on the HMD's DXGI
  adapter. All RGB is premultiplied. No scene renderer, encoding loop, texture
  readback, shader compilation or camera access runs in this helper.
- The public standing HMD pose is frozen at explicit launch. Eye-to-head offsets
  use the public eye transforms. Both original projection frusta are cropped
  linearly to normalized full-eye chart **[0.40,0.60] on both axes**, strictly
  inside v3's RGB interval `[560/3168,2608/3168]`, never its mask/guard region.
  The full-scene projection is not changed. Keep the head still while observing.
- Trusted, bounded metadata records the actual *recommended* eye dimensions,
  original/cropped frusta and frozen standing eye poses. Recommended dimensions
  are not proof of actual game allocation or delivered encoder resolution.
- Interior opaque black, transparent/fractional black, neutral gray, yellow/cyan
  bands, one/two white eye-ID blocks and genuine 1/2/4/8/16/32/64-texel detail
  periods are distinct witnesses. Seven 48-row detail bands begin at texture
  row1600. The coarse periods allow angular-frequency comparison even when this
  small overlay minifies; losing 1-texel stripes alone is not evidence of transport
  blur. This static 2048-square input does not establish 4K transport quality.

## Build without activating anything

```bash
bash pc/projection-overlay-probe-build.sh /path/to/reviewed/public/headers /private/output
```

Requires `g++`, `x86_64-w64-mingw32-g++` and reviewed `openvr_capi.h` SHA256
`3d32d8eb5bcbe2e250610123aee8142c68f0e7d4da823f87513b6a2a333078d7`.
The [official OpenVR SDK header](https://github.com/ValveSoftware/openvr/tree/master/headers)
and its [license](https://github.com/ValveSoftware/openvr/blob/master/LICENSE)
apply independently. No SDK/Valve binary is downloaded or redistributed.
The build executes only portable production-core/fake-API tests and cross-compiles
Win64 with warnings as errors. It does not execute Windows/D3D/OpenVR/Steam Link.

## Explicit launch and parent lifecycle

Only launch from a deliberately selected diagnostic action, with SteamVR and the
PC-VR stream already running, the dashboard closed and the head held still.
The managed child interface is:

```text
projection-overlay-probe.exe --stdin-pipe --openvr <absolute installed openvr_api.dll>
```

This requires a real stdin pipe **before** loading OpenVR. EOF, any byte or pipe
failure stops the child; `PeekNamedPipe` prevents a blocked read or reader thread.
A separate explicitly interactive mode accepts:

```powershell
.\projection-overlay-probe.exe --probe --openvr 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openvr_api.dll'
```

Enter, Escape or Ctrl+C removes both overlays. A 25ms poll is a cadence only:
there is **no automatic expiry or restart**. The helper polls a bounded number
of its own quit events, requests no input and is hidden in the dashboard.

The installed public DLL path must be canonical and absolute drive syntax,
identify a bounded PE64 `openvr_api.dll` and contain no reparse-point components.
Proton's absolute `Z:\...` mapping is accepted; actual Proton execution remains
unverified. The DLL stays open read-only, and loader search is limited to its
directory and System32. No substitute search or binary is supplied.

Both handles, cropped frusta and textures are prepared with overlay alpha=0;
both are shown before either alpha becomes1. After both reveals succeed and the
dashboard is still closed, stdout emits the **exact** line:

```text
SFNP_PROJECTION_READY_V2
```

The game may then latch its two-eye zero base. OpenVR has no atomic two-handle
publish: one frame of sequential per-eye reveal remains possible. This bounded
static diagnostic is not production stereo synchronization or per-frame reprojection.
The parent must latch its own zero-base change consistently across both eyes.

On EOF/manual stop every owned handle is hidden, cleared and destroyed before
SDK/D3D/DLL shutdown. Only a previously ready, cleanly stopped session emits:

```text
SFNP_PROJECTION_STOPPED_V2
```

Startup failures emit bounded
`SFNP_PROJECTION_REFUSED_V2 stage=<trusted-token> code=<number>` metadata, roll back
all owned handles and never report ready. Cleanup failure never reports stopped.
All stdout is ASCII, each line at most1024 bytes and total output at most8192
bytes, including errors. No device-supplied strings or serials are printed.
Abrupt child termination ends only its overlay ownership, not SteamVR.
No persistent rollback or Frame restart is necessary.

## Interpretation boundary

A successful public API return or ordinary colored pattern proves only that the
PC accepted an overlay. A positive interior black-versus-transparent witness with
a zero game base establishes surviving independent coverage under these exact
conditions; it does not identify the receiver buffer or establish arbitrary-game,
real-time scene, depth, latency, full-resolution or update-safe support. If only
flattened scene RGB changes and black cannot hide the camera, reject this route
as an alpha-preserving shortcut. A failure to display can also be an unsupported
Background/overlay path and must not be labeled transport flattening without
corroborating evidence. Existing scene frame/eye association requirements remain.
