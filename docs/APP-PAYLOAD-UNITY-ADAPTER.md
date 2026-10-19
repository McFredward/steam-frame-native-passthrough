# Unity application-output core

> **Historical reference.** Historical diagnostic API, not the current game integration. The static test-pattern/atlas source and its activation policy below are retained for reproducing the earlier experiment. Current games use the ordinary-RGBA [application source API](protocols/application-source-v1.md); see [Developing](DEVELOPING.md).

The four source files under `probe/unity` implement an isolated application-side
output core for the [same-frame payload experiment](APP-PAYLOAD-PROBE.md). They
are intended to be integrated **inside GloomhavenVR and controlled by its VR
settings**, not shipped as a separate BepInEx plugin. They contain no plugin
entry point, installer, native Steam sender hook or Frame service change.
Integration into the main mod and its settings is separate work; these files do
not claim that integration already exists.

This is still a **source-carriage diagnostic**. An armed component replaces one
existing camera's output with its eye's original static RGB/R8 fixture. It does
not yet export real game coverage, material opacity or MSAA. A static immutable
application content generation is repeated, never replaced with a submit count,
clock, Unity frame number or invented native video ID. Nothing here activates
passthrough or proves the selected decoded frame, composition, reprojection or
foveation mapping. See the independent
[association contract](EXACT-FRAME-ASSOCIATION.md) and
[no-native-sender-hook gates](PC-WITHOUT-SENDER-MODIFICATION.md).

## Reusable API and integration boundary

- `ProbeManifest` loads/checks the trusted source records, exact layout revision,
  stereo pair, immutable source generation, convention, eyes, extents and digests.
- `ProbeTexture.Load(absoluteDirectory, "left"/"right")` loads each original
  PNG once, validates its chunks/CRCs and bounded RGB8 dimensions before native
  decoding, then verifies actual decoded orientation, full canvas hash, all
  three header copies, identity/CRC, RGB/R8 hashes and exact grayscale coverage.
  `Dispose` destroys only the owned texture. Inputs are never loaded in a camera
  callback.
- `ProbePolicy` owns one explicit bounded run. `Start` requires one unique
  authorized camera, built-in rendering, active XR, MultiPass and both eyes.
  `Select` requires the same current camera and a real left/right callback;
  missing conditions return `Unknown`. `Stop`, clock failure, deadline expiry
  and component destruction permanently close that instance. An integrated
  settings controller may create a **fresh** policy for a new explicit enable,
  rather than mutating/resetting an old run's deadline.
- `PayloadOutput.Initialize(camera, policy, left, right)` attaches only to the
  existing named `GloomhavenVR.HeadCamera` object. The integration controller
  must find exactly one enabled active camera of that name, check supported
  built-in/MultiPass conditions, prepare both immutable textures and start the
  policy before adding/initializing one component. There is no automatic camera
  search, replacement attachment or rearm inside this component.
- On settings disable, the controller must stop the policy first, destroy only
  this component, then dispose the two owned textures. It must also handle
  camera replacement, scene changes, component/plugin destruction, deadline and
  preparation/callback failure. The game camera's own state is never replaced
  or reset by this core.

Use a clearly labeled experimental source-carrier/diagnostic setting, distinct
from genuine Mixed Reality availability, until native mask carriage and mapping
are proved. The diagnostic should start only from an explicit user action after
the ordinary VR stream is visible. Its existing 120-second bounded policy is a
diagnostic lifetime, not a future native passthrough feature requirement. No
standalone-plugin config, CLI flag or PC-key-only activation is required by this
core; the main mod owns the integrated setting and lifecycle.

## Rendering and observations

`PayloadOutput.OnRenderImage` admits only the same attached/current camera,
active built-in XR MultiPass/Both eyes, two-dimensional render targets and an
actual left/right `Camera.stereoActiveEye`. The callback uses public
`Graphics.Blit` with that eye's complete fixture. Unknown, mono, stopped or
unsupported callbacks preserve the ordinary source-to-destination blit. It saves
and restores `RenderTexture.active` and `GL.sRGBWrite`, and does not change
projection, pose, targets, culling, quality, XR configuration, UI or networking.
No custom shader, RGB key, readback, file access or network wait runs in the
render callback. Exceptions stop admission and are bounded for later reporting.

Three fixed slots retain the first fallback/left/right callback observations.
The main mod must drain them with `TakeObservation` from `Update` or another
non-rendering lifecycle point, and handle `TakeFault`. Routine callbacks only
increment saturated counters. Observations report actual/selected eye,
current-camera match, XR state/mode, built-in/both-eye gates, GL-write state,
color space, source/destination extents, formats, dimensions, VR usage and sRGB
flags. A null destination is explicitly unknown backbuffer metadata. The owning
controller should also make one bounded camera image-effect census, because
subsequent image effects or native composition can change the eventual output.

Unity documents `stereoActiveEye` as meaningful in image-effect callbacks and
`OnRenderImage` as built-in-pipeline functionality. Neither this public contract
nor a callback observation proves final native stream pixels.
[Unity stereoActiveEye](https://docs.unity3d.com/2021.3/Documentation/ScriptReference/Camera-stereoActiveEye.html),
[Unity OnRenderImage](https://docs.unity3d.com/2021.3/Documentation/ScriptReference/MonoBehaviour.OnRenderImage.html),
[Unity Graphics.Blit](https://docs.unity3d.com/2021.3/Documentation/ScriptReference/Graphics.Blit.html).

The texture is RGB24, non-mipmapped, point-filtered, clamped and non-readable,
created with `linear=false`. The core accepts only straight RGB source records,
source dimensions 32 through 512 per axis, 8 MiB per PNG and 8 KiB per manifest.
It rejects observed file/directory links and inconsistent inputs. The local
private source directory is trusted input, not isolation against a malicious
process with the same filesystem rights. Source digests are reference equality,
not remote authentication.

## Local compile and controls

The optional **compile-only** project has no BepInEx entry point and no NuGet
packages. Supply existing read-only Unity and .NET Framework 4.7.2 reference
directories; reference DLLs have `Private=false` and must not be redistributed.
`NuGet.Config` clears package sources and automatic framework packages are off.

```bash
dotnet build probe/unity/AppPayloadCore.csproj \
  --configfile probe/unity/NuGet.Config -c Release \
  -p:UnityReferenceDir=/absolute/game/Gloomhaven_Data/Managed \
  -p:FrameworkReferenceDir=/absolute/local/net472/reference-directory \
  -p:BaseIntermediateOutputPath=/absolute/private/core-build/obj/ \
  -p:OutputPath=/absolute/private/core-build/bin/
dotnet build probe/unity/tests/PortableControls.csproj \
  --configfile probe/unity/NuGet.Config -c Release \
  -p:BaseIntermediateOutputPath=/absolute/private/policy/obj/ \
  -p:OutputPath=/absolute/private/policy/bin/
dotnet /absolute/private/policy/bin/PortableControls.dll \
  /absolute/private/original-payload-directory
```

`tests/UnityRenderControls.cs` runs real Unity 2021.3.5 Play Mode rendering in an
isolated project. Copy it to `Assets/Editor` and the four production core files
to `Assets/Probe`. Include only installed built-in packages
`com.unity.modules.imageconversion`, `com.unity.modules.imgui`,
`com.unity.modules.vr` and `com.unity.modules.xr`. Run the editor with
`-batchmode -force-glcore -executeMethod UnityRenderControls.Run`,
`-probeSourceDirectory /absolute/payload`,
`-probeOutputDirectory /absolute/private/results` and an isolated `-projectPath`.
The fixture enters actual Play Mode because ordinary runtime image effects need
not execute in editor Edit Mode. It compares the camera's actual no-component
baseline with the component's mono fallback, rather than guessing clear-color
RGB codes across color spaces.

The 2026-10-15 software evidence on Unity `2021.3.5f1`, OpenGLCore/llvmpipe:

| Scope | Observed result |
| --- | --- |
| Portable policy/manifest controls | 584 checks passed; no Unity/native rendering claim |
| Gamma actual rendering | 20 controls passed; loaded eye orientation and GPU-blit RGB hashes exactly matched source; four RT/GL paths had zero RGB-code error |
| Linear actual rendering | 18 controls passed; sRGB RT plus `GL.sRGBWrite=true` preserved exact RGB bytes; other tested paths had MAE `24.2243`, max error `73` |
| Actual mono callback in both color spaces | `OnRenderImage` invoked, `Camera.current` matched, source fallback equaled the ordinary baseline, GL/RT state restored, destruction stopped the policy |

The late bounded exception-reporting repair and the removal of the abandoned
standalone entry point do not change the successfully exercised blit, texture
loading or normal admission paths; their focused compile/policy evidence is
recorded separately. Actual XR left/right callbacks, D3D output, native codec
carriage, decoded-resource association, nonlinear head/gaze/foveation mapping and
fractional-alpha survival remain **unproved**. Preserve this distinction when
integrating the main mod setting or planning the next hardware experiment.
