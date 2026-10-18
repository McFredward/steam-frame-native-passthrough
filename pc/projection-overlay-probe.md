# Public projection-overlay transport probe (unverified on hardware)

This development helper asks one narrow question: does Steam Link transport a
public per-eye projection overlay's independent alpha to the Frame compositor,
or does the PC compositor flatten it into the scene video first? It is **not a
passthrough implementation**, full-resolution fix, product option or installer.
It performs no Steam setting writes, receiver install, restart or autostart.

The source uses Valve's public `IVRSystem_026` and `IVROverlay_028` function
tables. `SetOverlayTransformProjection` takes the actual eye's frustum and pose;
this is not a flat quad pretending to be a scene. Both static 512-square RGBA8
textures are allocated once on OpenVR's DXGI adapter. There is no scene renderer,
camera, encoding loop, texture readback or shader compilation. The image's pose
is frozen at launch for this transport test; it is not a real-time scene cohort.

## Build without activating anything

Use the reviewed public `openvr_capi.h` (SHA256
`3d32d8eb5bcbe2e250610123aee8142c68f0e7d4da823f87513b6a2a333078d7`).
It is not vendored or downloaded by the build. Obtain it from the official
[OpenVR SDK](https://github.com/ValveSoftware/openvr/tree/master/headers),
whose [license](https://github.com/ValveSoftware/openvr/blob/master/LICENSE)
applies independently. A different header requires ABI review before updating
the pin. No Valve binary is included in this project or diagnostic output.

```bash
bash pc/projection-overlay-probe-build.sh /path/to/reviewed/public/headers /private/probe-output
```

The build runs only portable lifecycle/pattern checks and cross-compiles Win64.
It does not establish Windows/D3D execution or Steam Link transport behavior.
Keep the small build receipt and current EXE; remove the temporary host test
executable after its process exits.

## Future explicitly coordinated hardware check

Do not run until the matching single receiver observation is prepared. SteamVR
and the VR stream must already run; this helper refuses without `vrserver.exe`.
On the PC, use its installed OpenVR DLL with an absolute path:

```powershell
.\projection-overlay-probe.exe --probe --openvr 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openvr_api.dll'
```

Left is yellow, right cyan, with different spatial eye marks, a transparent
border, four exact alpha bands (63/127/191/255), opaque black, and one-pixel
detail bars. Alpha is premultiplied. The helper reports eye handles, interface
versions, frusta, frozen eye transforms, dimensions and UTC start to stdout.
Enter, Escape or Ctrl+C hides, clears and destroys both overlays and shuts down
only this OpenVR client. There is no timeout. The Steam dashboard remains
available; the overlays request no interaction and hide in the dashboard.
Closing/terminating the helper process also ends its overlay ownership, but the
ordinary manual stop gives a visible cleanup receipt. No persistent rollback is
necessary. Never stop SteamVR merely to remove this diagnostic.

## What must be established before choosing this route

- A receiver capture must identify an independently delivered RGBA texture or
  equivalent true-alpha surface with this exact eye pattern, size and alpha.
  Seeing translucent colors blended over the PC scene is **not** that proof.
- If the pattern exists only inside the opaque encoded scene, reject this API
  as an independent-alpha shortcut. Public function availability, a successful
  return, a local PC overlay or laser metadata do not prove separate transport.
- If an independent image exists, prove its remote pose/frustum, sharp alpha,
  per-eye identity, update latency and actual delivered resolution. Then prove
  real per-frame scene registration and fallback separately.
- OpenVR does not offer atomic two-overlay publication in this interface. This
  helper prepares both before showing either and rolls back every failure, but
  sequential visibility is acceptable only for this explicit static diagnostic.
  It is not the production both-eye coherence proof.
- No input scene image is submitted through this helper; therefore this test
  alone cannot certify scene depth, current-frame scene reprojection, arbitrary
  game compatibility, Proton execution or preserved full-resolution rendering.

The existing code's independent scene coverage contract remains unchanged.
