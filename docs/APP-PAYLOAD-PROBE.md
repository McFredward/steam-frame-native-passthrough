# App-owned same-frame payload probe

> **Historical reference.** Historical offline atlas experiment, not the installed coverage route. Its image packing, test actions and missing-adapter status describe that checkpoint only. Current transport uses the [coverage certificate](protocols/stream-coverage-v1.md) beside unchanged video; start with [Developing](DEVELOPING.md).

This is an **offline source fixture**, not working Steam Link passthrough. It
prepares the first carriage experiment from the
[no-native-sender-hook plan](PC-WITHOUT-SENDER-MODIFICATION.md). It changes no
Gloomhaven code, Steam installation, Frame file, process, shader or runtime
setting. No additional sender modification is required to generate its files.
The missing application output adapter and a separately approved receiver capture
are still required before an actual transport test can happen.

`tools/app_payload_probe.py` uses only Python's standard library on POSIX hosts;
its bounded file handling requires `O_NOFOLLOW` and `O_NONBLOCK`. The Windows
GloomhavenVR integration does not execute this offline producer. It accepts
owned, immutable RGB8 color and R8 coverage buffers from **one app generation and
one eye**. Coverage is never classified from scene RGB. It writes a normal RGB8
PNG carrying distinct color and grayscale coverage regions, plus a small JSON
source record. Each eye has its own complete canvas; the other eye is never
used as the mask plane. Coverage occupies real submitted image area: there is
no hidden alpha plane or promise that extra texture bounds survive Steam Link.

## Generate original reference fixtures

Use an empty private output directory. The session below is an example; a real
source must create a fresh nonzero random 128-bit session on reconnect/restart
and assign an immutable application-owned content generation. Never reuse a
session/content/eye tuple for changed color, coverage, extent, convention or
layout. The number is **not** a native submit counter, encoder ID, presentation
index or proof of a selected decoded frame.

```bash
mkdir -m 700 build/app-payload-reference
python3 -B tools/app_payload_probe.py fixture \
  --width 320 --height 192 --session 0123456789abcdef0123456789abcdef \
  --content-id 7 --eye left \
  --png build/app-payload-reference/left.png \
  --record build/app-payload-reference/left-source.json
python3 -B tools/app_payload_probe.py fixture \
  --width 320 --height 192 --session 0123456789abcdef0123456789abcdef \
  --content-id 7 --eye right \
  --png build/app-payload-reference/right.png \
  --record build/app-payload-reference/right-source.json
```

The original fixture has independent asymmetric left/right coordinate patterns,
opaque and transparent scene colors, 0/25/50/75/100-percent coverage, and an
additional two-layer 50-percent-over-50-percent coverage example. Its RGB does
not select opacity: red and green occur at different coverage values. The R8
swatches are `0, 64, 128, 191, 255`; two already quantized `128/255` layers compose
to approximately `192/255`. This fixture is not an MSAA, material-alpha or Unity
rendering test, and its synthetic RGB is declared straight rather than
premultiplied.

To use **actual owned source buffers**, use `generate` instead of `fixture` and
add `--color-rgb source.rgb --coverage-r8 source.r8`. The files must contain
exactly `width * height * 3` tightly packed RGB bytes and `width * height` linear
coverage bytes respectively. `--rgb-convention straight` is the default;
`premultiplied` declares supplied RGB as already premultiplied. The tool does
not convert color spaces or repair the source's transparency semantics. Color
code values are opaque bytes to this diagnostic; coverage uses linear R8
opacity. A future app adapter must declare and test the actual render-target
transfer convention, including partially transparent materials and MSAA edges.

Files are created with mode `0600`, never overwritten, and input symlinks or
nonregular files are rejected. The caller chooses the parent directory, which
must already exist and should be private. The source extent is bounded to
`32..2048` pixels on each axis; the packed canvas is at most `4144 × 2404`.
Inputs, PNG chunk count, decompression and output pixels have explicit limits.
There are no queues, sockets, device access, subprocesses or renderer callbacks.

## Layout and data header

Revision 1 places three complete high-contrast complementary-bit headers above
the two source planes. For each data bit a `4 × 4` black/white cell is followed
by its white/black complement. Each header has 64 bits per row. Asymmetric
corner keys and coordinate rulers sit **outside** both supplied source planes.
A bottom reference panel carries the coverage swatches and an eye-specific
coordinate grid. The source record gives exact pixel rectangles; no receiver
may infer a nonlinear spatial mapping from those source rectangles alone.

The big-endian header is 132 bytes: the 128-byte structure
`>8sHH16sQBBH12H32s32s`, followed by CRC32 of that structure. Fields, in order:

| Field | Meaning |
| --- | --- |
| 8 bytes `FAPPROB1`, uint16 version `1`, uint16 layout revision `1` | Exact diagnostic format |
| 16-byte nonce, uint64 content generation | App-owned source identity |
| uint8 eye, uint8 RGB convention, uint16 zero | Left `0` / right `1`; straight `0` / premultiplied `1`; reserved zero |
| Twelve uint16 values | Source width/height, canvas width/height, color x/y/width/height, coverage x/y/width/height |
| 32-byte color SHA-256, 32-byte coverage SHA-256 | Digests of the original tightly packed source planes |
| uint32 CRC32 | Header corruption detection; not authentication |

Three matching copies, strong cell contrast and CRC permit a bounded header
recovery experiment. They are **not evidence of robustness through the native
codec**. Digests establish exact reference equality, not cryptographic trust in
a remote image. A trusted local generation manifest is mandatory for validation,
so a newly authored image reusing the same content ID with changed bytes cannot
replace the expected source. An older recorded frame is rejected when a newer
expected content generation is requested; there is no "latest available" fallback.

## Validate and measure, with opaque fallback

```bash
python3 -B tools/app_payload_probe.py validate \
  --width 320 --height 192 --session 0123456789abcdef0123456789abcdef \
  --content-id 7 --eye left \
  --png build/app-payload-reference/left.png \
  --source-manifest build/app-payload-reference/left-source.json \
  --record build/app-payload-reference/left-validation.json
```

The strict validator checks all three headers, CRC, requested session/content/
eye/extent/convention, immutable source digests, both source planes, corner keys,
reference swatches and the complete deterministic canvas. Header damage,
clipping, mirror/rotation, changed extent, wrong source generation/session/eye,
missing expected source record and plane changes reject the reference.
`reference_valid: true` means **only exact app-owned source equality**.
Every result, including success, retains `opaque_fallback_required: true` and
`passthrough_authorized: false`; native frame identity and spatial mapping remain
unknown. There is deliberately no activation output or passthrough image.

The PNG decoder supports bounded noninterlaced RGB8 PNGs with all five standard
row filters. It rejects invalid CRCs, truncated/trailing data, decompression
bombs, unknown transformations/profiles and non-RGB8 input. Convert a captured
image explicitly to raw RGB8 code values outside this tool if needed, retaining
that conversion's provenance. Unsupported image encodings cannot authorize a
mask.

For a future decoded RGB8 capture at the **unchanged source canvas extent**, run
`measure` with the same arguments, a new output record, and
`--reference-png build/app-payload-reference/left.png`; set `--png` to the
candidate. It reports whether all three encoded headers can still be recovered,
and color/coverage-region RGB-code mean-absolute and maximum errors against the
exact reference. These are source-coordinate comparisons only. Resized captures
are rejected for these metrics instead of guessed into alignment. Header
recovery and small numeric errors do not authorize native presentation or prove
coverage registration. Large RGB/R8 errors can therefore be diagnosed separately
from header survival without weakening the strict reference gate.

## Next real experiment and stop conditions

1. Build an **app-owned output adapter** which packs genuine same-generation
   color and coverage into the submitted output, plus the source header. A
   static PNG viewed as a scene is insufficient evidence for moving real alpha.
   Record render pose/projection, transfer convention and layout at the source.
2. With separate approval, inspect one selected decoded resource containing
   **both** planes and recover the complete source identity there. Do not join
   a header from one image to a mask from another, or substitute a resource
   allocation generation, timing guess, submit number or freshest mask.
3. Establish composition and the actual reprojection/foveation mapping at both
   packed placements during head and gaze movement. Equal-frame membership,
   asymmetric fiducials and apparently aligned static pixels are insufficient.
   Reprojection can transform the two regions at different rays; a constant
   post-warp offset cannot repair a nonlinear transform. Unknown dashboard/
   notification composition or synthetic frames must remain opaque.
4. Test repeats/drops, reconnect, wrong session/eye, resolution/layout changes,
   partial clipping, codec damage, moving gaze and unknown layers. A layout or
   transform revision invalidates earlier calibration. Require zero accepted
   wrong-source associations before any presentation work, then measure alpha
   error, edge displacement, latency and processing cost against agreed limits.

If full-plane carriage fails, or composition/spatial mapping cannot be proved
through the untouched native sender, stop this route. Additional unrelated YUV
metadata captures do not solve that boundary. An owned renderer/transport or a
public Valve alpha contract is then required. See
[the exact-association contract](EXACT-FRAME-ASSOCIATION.md) for the independent
GPU-resource lifetime and activation gates.

Focused controls:

```bash
python3 -B -m unittest discover -s tests -p test_app_payload_probe.py -v
```

These controls establish only the software fixture, strict reference rejection,
numeric reporting and bounded local I/O. They cannot establish codec survival,
selected native frame association, headset picture, nonlinear UV mapping,
translucent materials, Steam Link behavior or performance.
