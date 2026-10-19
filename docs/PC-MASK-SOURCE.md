# PC source for a real Steam Link mask

> **Historical reference.** Historical source investigation. The no-PC-sender-modification preference below was superseded by the maintainer's explicit authorization of the paired sender companion on 2026-10-18. The atlas investigation is retired, not a fallback. Current integration starts at [Developing](DEVELOPING.md).

Investigation: 2026-10-11. The maintainer subsequently supplied the installed
Windows driver and manifest for private static inspection; see
[the actual PC driver dataflow](PC-DRIVER-20261011.md). A live Frame inspection is recorded separately in
[the runtime report](FRAME-LIVE-20261011.md). No headset file, setting or process
was modified by these investigations. This document selects
the next source investigation; it does not describe an installable bridge.

## Decision

The maintainer's 2026-10-12 preference is to avoid any additional modification,
injection or private hook in the PC Steam sender. Application coverage export
remains necessary. Investigate app-owned explicit-mask/data transport through
the unchanged video path first; do not assume a separate channel has an exact
video association merely because both endpoints receive data.

The [no-sender-modification review](PC-WITHOUT-SENDER-MODIFICATION.md) ranks the
preferred experiments, measurable compromises and stop conditions.

The installed-driver inspection below remains useful source evidence and a
possible fallback investigation. It does not authorize a PC sender hook.

Inspect the **installed PC vrlink driver** before considering such a hook. The
useful interception point is its actual encoder input: composed stereo color,
coverage and source identity must belong to that same input. Polling the public
mirror cannot establish this association. A second display-redirect driver is
also not a transparent observer of the existing Steam Link driver.

The supplied driver's actual HMD component dispatcher exposes
`IVRDriverDirectModeComponent_009` and `IVRDisplayComponent_003`; it does not
expose `IVRVirtualDisplay_002`. The latter's presence as a binary string was not
proof of an active component. Its final encoder texture is NV12/P010, which has
no scene-alpha channel. The native transmitter also assigns its own 16-bit frame
key after encoded output is queued with metadata. Do not equate that key with
the metadata presentation number. A coverage pass must remain associated with
the original composition through this later key assignment.

### Public interfaces establish these boundaries

| Candidate | Available information | Missing contract |
| --- | --- | --- |
| `IVRCompositor::GetMirrorTextureD3D11` | Undistorted composite SRV for one eye; paired release API | No returned frame identity, atomic relation to an encoder input, or documented preservation of scene alpha |
| Application `Submit` / `SubmitWithArrayIndex` | Eye texture, bounds, optional render pose/depth | No encoder ID; later composition, reprojection and repeated presentation can differ from application submissions |
| `IVRVirtualDisplay_002::Present` | Final backbuffer handle and `PresentInfo_t::nFrameId` | Whether deployed vrlink uses this interface, retains meaningful alpha, or carries this identity into decoded metadata |
| `IVRDriverDirectModeComponent_009` | Per-layer eye handles, bounds, projection and render pose; `Present` synchronization texture | No equivalent public encoder-frame ID in those arguments; driver-specific composition must be inspected |

These findings come from the [current public OpenVR header](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr.h),
[driver header](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_driver.h)
and [driver API documentation](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/docs/Driver_API_Documentation.md).
The public API does not establish that the mirror's alpha is usable; equally,
absence of that guarantee does not prove that its runtime alpha is always opaque.

Valve's [virtual-display sample](https://github.com/ValveSoftware/virtual_display/tree/da13899ea6b4c0e4167ed97c77c6d433718489b1)
is from 2017 and implements an older `Present(shared texture handle)` signature.
It demonstrates registering a `DisplayRedirect`, not adding a streaming tap.
Do not copy that ABI into a proxy for a modern driver. Valve's current
[Steam Link vendor integration](https://github.com/ValveSoftware/openvr/wiki/Steam-Link-Integration-for-External-Vendors)
documents resource-only drivers and foveation configuration, not an alpha-plane
export or arbitrary encoder metadata extension.

## GloomhavenVR also needs a real-alpha source

The current mod deliberately makes PC chroma backgrounds fully opaque:
`src/GloomhavenVR/Core/MixedReality/MixedReality.cs` selects transparent clear
only for the native Frame path and sets `key.a = 1f` otherwise. Extracting this
existing PC image's alpha is therefore insufficient.

The future PC bridge needs an explicit alpha-export rendering mode: transparent
eye clear, coverage-preserving opaque/transparent shaders and a declared straight
or premultiplied RGB convention. Reuse the native coverage handling where
applicable; do not select an unsupported PC OpenXR alpha-blend mode merely to
generate coverage. Application color and coverage must enter the same composition
and reprojection before export. SteamVR dashboards and unrelated applications
remain opaque. No mod code was changed for this investigation.

Fractional opacity must survive the source rendering and every later layer
composition. Include tinted translucent glass over both transparent clear and
opaque virtual geometry, plus overlapping translucent surfaces, in the source
test. Two 50-percent layers over transparent clear have 75-percent combined
opacity. The existing R8 transport and straight/premultiplied shader tests
support intermediate values; they do not prove Unity material alpha or glass
in the final Steam Link stream.

## Smallest useful PC evidence

From the **SteamVR installation used to stream**, privately supply:

1. `drivers/vrlink/bin/win64/driver_vrlink.dll` and
   `drivers/vrlink/driver.vrdrivermanifest`.
2. SteamVR version/build, driver SHA-256 and a relative-path/size inventory of
   `drivers/vrlink/resources/`. Copy only the relevant encoder/composition shaders
   subsequently identified from this inventory. If shader code is embedded in
   another module, identify that dependency before requesting it.
3. The relevant connection/encoder initialization excerpt from a short successful
   Steam Link run, with headset name, eye/video extent and graphics backend.
   No complete user configuration, credentials or memory dump is needed.

Typical SteamVR root:
`C:\Program Files (x86)\Steam\steamapps\common\SteamVR`. A custom Steam library
has the same `drivers/vrlink/` layout underneath its SteamVR directory. Keep all
binary/shader copies in an ignored private directory; never publish them.

Run the portable collector from a checkout of this fork on the PC:

```powershell
py -3 tools/collect_pc_bridge.py --steamvr-root "C:\Program Files (x86)\Steam\steamapps\common\SteamVR"
```

It prints a redacted JSON report with driver hashes, candidate interface-version
strings and a bounded shader inventory. A string in a binary is not proof of an
active interface or its ABI. It does not read user settings, registry, environment
or logs and never loads the driver or starts SteamVR. File reads are capped at
64 MiB each and 128 MiB total; missing/oversized files are reported explicitly.

To also create the requested minimal private driver/manifest copies, add
`--private-output build/pc-bridge-private-1`. Use a new directory outside the
SteamVR installation. Shader bodies are not copied. Keep the private directory
local; share its content only for this investigation. Unknown builds always
report alpha and encoder/decoder association as unverified.

First inspect supported component names, encoder dependencies and shader alpha
handling statically. Then a narrowly scoped PC trace should record the actual
component call, resource handle **plus reuse generation**, encoder-source ID,
emitted metadata ID and GPU synchronization. An alpha-capable texture format is
not proof of useful coverage: use an original test scene with opaque red, opaque
green, transparent black and partial coverage and compare source/output alpha.
This test must not derive coverage from RGB. The returned identity must then be
correlated with the decoded-surface association described in
[the Frame decoder investigation](FRAME-DECODER-20261011.md).

## Concrete implementation branches

- **Verified existing-driver source:** export coverage at the actual composed
  encoder input and carry its verified ID through the existing decoder metadata
  association. Use bounded GPU copies/staging and a worker, with opaque fallback
  on any missing identity. If alpha has already been discarded, preserve it in
  that composition step or render a parallel coverage pass with the exact same
  layers and transforms; a mirror readback cannot recreate it.
- **Same-frame packing:** put an explicit coverage plane alongside color in the
  same encoded video frame. This removes a separate mask/video arrival pairing,
  but still needs access to real alpha before encoding and verified eye/crop/
  foveation mapping. Compression of the grayscale plane is a measurable quality
  tradeoff, not exact lossless R8 transport.
- **Controlled streaming renderer:** if vrlink exposes no usable source or
  metadata route, control composition, encoding and receiving together in a
  separate transport. This gives the project ownership of frame IDs and alpha
  lifetime, but is a larger integration than a compositor shader replacement.

Do not substitute a mirror poll timestamp, compositor timing index, Vulkan
submission counter or the newest arriving mask for the missing encoder link.
The camera stays on the Frame in every branch; no camera upload is required.

Focused collector validation (original synthetic fixtures, no runtime needed):

```bash
python3 -m unittest discover -s tests -p 'test_collect_pc_bridge.py' -v
```
