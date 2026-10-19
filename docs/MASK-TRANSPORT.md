# Explicit stereo-mask foundation

> **Historical reference.** Historical 2026-10-11 UDP/CRC foundation. The installed route does not use this mask protocol or its activation/expiry policy. It uses authenticated [coverage certificates](protocols/stream-coverage-v1.md) through Steam Link. Statements below about a missing bridge describe the original checkpoint; see [Developing](DEVELOPING.md) for current integration.

Status: implemented and software-tested, 2026-10-11. This is a development path
for real scene alpha, independent of the scene's RGB. It is **not yet a working
Steam Link mask integration**. The existing chroma path remains separate.

## Implemented

`mask/mask_protocol.{h,cpp}` exports a portable C++17 encoder and bounded receiver.
Each mask identifies a 128-bit session, monotonically increasing 64-bit source
frame, eye and pixel extent. It contains row-major R8 coverage: 0 means camera,
255 means virtual scene, intermediate values preserve antialiased edges and
fractional material opacity, such as glass. Its
coordinate convention is upper-left origin, matching the source-eye texture's
normalized valid rectangle; a later adapter must prove the native mapping.

Raw and run-length payloads preserve all eight bits. UDP-sized fragments are at
most 1200 bytes, with explicit network byte order and packet/full-frame CRC-32.
The header and constants are documented in the public C++ header. Independently
fixed wire vectors and Python's CRC implementation test interoperability, rather
than checking only this encoder against its own decoder.

The receiver binds a session explicitly, limits allocations and partial/cached
frames, rejects conflicting fragments, and expires incomplete/stale traffic.
Retirement history prevents consumed or discarded frames from resurrecting.
Reordering is bounded; source IDs cannot restart or wrap within the same session.
Call `beginSession` on reconnect with a fresh, cryptographically random session ID.
Monotonic-clock regression discards pending state and unbinds the session; a fresh
explicit handshake is required before another mask can be accepted.
CRC and session matching provide integrity/association, **not authentication**:
the eventual streaming adapter needs a trusted authenticated transport.

`takeExact` returns coverage only for the supplied session/frame/eye/extent.
It consumes the cached CPU frame; a renderer must retain the matching GPU image
for repeated reprojection of that same decoded video frame, and retire it only
after all GPU users have finished. No match means opaque rendering. It never
substitutes an older, newer or other-eye mask.

`tools/gen_mask_spv.py` patches the supported final scene output to sample a
separate R8_UNORM texture and write premultiplied scene output. Its explicit
`--rgb-convention straight|premultiplied` option must match the proven encoder
input: `straight` multiplies RGB by coverage once; `premultiplied` preserves RGB
at nonzero coverage and clears RGB at zero coverage. This avoids multiplying
already premultiplied edges a second time. The shader does not infer the convention
from pixel colors. Existing source
UV, color conversion and reprojection instructions are retained. There is no
RGB threshold or spill suppression in this path. It rejects unfamiliar layouts
and occupied descriptor bindings, runs real SPIR-V validation, and atomically
replaces output only after success. Set 0/binding 31 is a test default, **not a
verified SteamVR pipeline binding**. Use linear filtering, opaque white borders,
no mipmaps and a linear coverage texture; do not gamma-correct coverage.

Generated shaders carry an `MRMASK/1` module marker. The rebuilt fork's legacy
compositor layer refuses these overrides because it does not yet supply their descriptor
or source-frame mapping. It also rejects changed descriptor bindings relative
to the original shader, including mask modules with stripped debug markers.
Shader-module creation success alone would not validate the pipeline's descriptor
layout. Old installed upstream layer binaries do not contain this guard; never
configure a mask override there. Removing the guard is not an installation step.
No compositor hook, camera setting or game MR setting is enabled by tests.

## Missing runtime adapter

The PC exporter must obtain coverage from the same stereo rendering used by the
video encoder, including any PC compositor composition. Exporting an application's
alpha alone is insufficient if video contains a later reprojected/composited frame.
The headset must associate that exact source identity with each reused decoded
surface and select the correct eye, crop and foveation mapping for every draw.

The current fork exposes neither this link nor a verified mask descriptor/texture
binding. A Vulkan submit counter and a timestamp are not substitutes. See
[the bridge investigation](MASK-BRIDGE-INVESTIGATION.md),
[the actual PC DirectMode/TX path](PC-DRIVER-20261011.md) and
[the conditional full-eye UV route](FRAME-UV-BINDING-20261011.md). The inspected
PC HMD does not expose the initially considered `IVRVirtualDisplay` component.
The later adapter must explicitly verify/negotiate UV and RGB/alpha conventions
for the session; packet dimensions alone do not establish them.

The runtime adapter must also handle bounded worker-thread reception, reusable
GPU staging/images, layout transitions, queue/semaphore ordering, frame-in-flight
ownership, reconnects and resolution changes. It must avoid blocking or allocating
on the compositor render thread. The camera stays local to the headset.

Production transport also needs a measured bandwidth budget. Raw 3408×3408
stereo coverage at 90 Hz would require roughly 2.09 GB/s (16.7 Gbit/s), before
headers. Run-length encoding helps sparse coverage but is not a guaranteed
production compression ratio. Measure actual game masks and evaluate lossless
compression, bounded delta/keyframe transport or same-frame in-band packing.
Any optional lower-resolution mask must have an explicit quality tradeoff;
do not silently reduce coverage quality or assume the diagnostic sender can
sustain full-resolution streaming.

## Build and verify on a development machine

Required: C++17 compiler, CMake, Python 3, Vulkan development headers, `glslc`,
`spirv-as` and `spirv-val`. The GPU test uses Mesa lavapipe when installed; it
executes the **actual patched fragment SPIR-V**, not just a CPU approximation.
It checks multiple RGB colors, partial coverage and opaque fallback pixels.
The fixture is original test code; no Valve shader is redistributed.

```bash
cmake -S . -B build/mask -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/mask --parallel 4
ctest --test-dir build/mask --output-on-failure --parallel 4
```

This also compiles the legacy Vulkan layer after its explicit-mask guard change.
Transport-only builds, including Windows/MinGW, can use `-DBUILD_TESTING=OFF`.
On Windows the socket endpoint links `ws2_32`. This does not replace the headset's
existing `build.sh`/`install.sh` or install a streaming mask feature.

## Diagnostic socket endpoint

`build/mask/mask_endpoint` sends/receives binary P5 PGM coverage images. Its
expected identity is supplied manually: it **does not identify Steam Link frames**.
The loopback tests exercise the actual sender/receiver with multi-packet masks,
exact matching, independent wire parsing and wrong-frame/eye/session controls.

For a local diagnostic, create a 4×1 P5 file with coverage bytes `[0,64,128,255]`.
In separate terminals, start reception and then send the file:

```bash
build/mask/mask_endpoint receive 127.0.0.1 47831 0102030405060708090a0b0c0d0e0f10 42 0 4 1 build/received.pgm
build/mask/mask_endpoint send 127.0.0.1 47831 0102030405060708090a0b0c0d0e0f10 42 0 build/coverage.pgm
```

The fixed example session is only for local tests. Missing matches time out
without producing an output mask; receivers never guess a different identity.
The endpoint is a one-frame diagnostic, not a continuous 90 Hz production
sender, secure service or benchmark of Frame streaming latency/bandwidth.

## Validation recorded for this checkpoint

- Protocol: 8,482 assertions, adversarial/mutated traffic and randomized payloads;
  separate AddressSanitizer/UndefinedBehaviorSanitizer run passed.
- Shader: 44 focused tests with real assembly, validation and refusal controls,
  including original separate-image/sampler resources that expose native type
  reuse. Private generation of the inspected Frame shader passes `spirv-val`
  for both RGB conventions; this establishes assembly compatibility only.
- Collector: 15 focused tests, including a real synthetic shader tool roundtrip.
- Root integration: real UDP roundtrip/wrong-identity controls, generated shader
  GPU rendering with straight and premultiplied input, legacy/stripped-module
  override guards, and legacy layer compilation. Windows sender cross-compilation
  passed; Windows runtime execution was not tested.

No headset picture, Steam Link frame association, streaming latency, or
GloomhavenVR MR behavior has been verified at this checkpoint.
