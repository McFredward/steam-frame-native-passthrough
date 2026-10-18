# Actual producer → lossy codec → native stereo regression

This explicitly offline fixture consumes the six **actual compiled Unity** source
and carrier PNGs, their `codec-inputs.json` pins and `gpu-result.json` probes. It
never replaces the images with analytic guards or measured twelve-value vectors.
The producer input must be 4040-square, actually resolved from 8-sample rendering.

```sh
python3 -B tests/native-fragment-robust-scene-stream/run.py \
  --producer /private/actual-unity-producer-run \
  --original /private/distort_ps_achromatic_se2.spv \
  --candidate /private/shipping-robust.spv \
  --historical-dual /private/quarantined-dual.spv \
  --record /private/exclusive-output/result.json
```

An explicitly available Khronos validation layer is required. `--driver intel`
is the default; `software` selects Lavapipe. The public CTest
`native_fragment_robust_scene_stream_contract` runs only four finite harness
math/bounds controls, with no private pixels, codec or GPU dependency. The separate
native worker's seven-method test is registered as
`native_fragment_scene_robust_presenter`.

## Boundaries and oracles

The finite codec matrix is H.264/HEVC, QP23/32, bilinear chart reduction to
1152-square and 576-square, BT.709 limited-range 4:2:0 encoding and explicit
full-range RGB decoding. Each eye chart is encoded independently as one lossy
intra frame. This models the known compression-sensitive condition, **not** the
proprietary Steam Link encoder, its full stereo-frame rate control, motion,
bitrate, chroma siting, current foveation or current physical-eye routing.

Decoded RGB is **not** passed directly to the native shader. Every texel is
inverse-encoded into the exact original non-YCoCg matrix's packed `(Cr,Y,Cb)`
components. This preserves the decoded RGB under the original decoder and gives
neutral numeric coverage/header cells the corresponding raw Y. The complete two
eye images occupy R1 quarters `[0,.25,1,.5]` and `[0,.75,1,1]` in one texture.
The actual textures are 1152×4608 and 576×2304 respectively; the smaller chart is
not reinterpreted as a 1152 texture. Original selector0/1 and the explicitly
controlled original flag route `0x02038018` select those quarters. Inner
reconstruction is disabled by that test metadata. These retained-chart controls
are not a measurement of the failed hardware run's current R1 or a validation of
other native reconstruction routes.

The private exactly pinned original and shipping v3+v1 fragment execute on
Vulkan. Fifteen original producer probes per eye compare the actual output to
both the full post-codec panel image and the original source RGBA. The latter
has a declared four-code source-quality budget; hardware interpolation uses
only the reported `subTexelPrecisionBits` bound. The geometry, tile seams,
constant .25/.5/.75 glass and positive E at A=0 witnesses must work in **both**
eyes. Real ONE/ONE_MINUS_SRC_ALPHA composition is also checked. A separately
executed original pipeline is restored bit-exactly. Two additional per-eye
ordinary-image/OFF → atlas recovery controls reuse existing recorded commands.

A guard refusal is not accepted as a passing advertised atlas stream. The
coupled criterion requires every eye's scene/coverage witnesses to match. An
opaque uniform pixel can numerically match both paths, so exact-original pixel
counts are diagnostic and are not claimed to reveal the guard branch. Fractional
and emission witnesses make the complete eye criterion decisive.

## Retained run of 2026-10-18

The actual producer bank was
`cb9fc34466df92c64f53f10f5ef3a64b9168de8933d5aaa0b3cf8c61cc6e5bf0`.
The shipping robust module was 31384 bytes,
`44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49`.
All eight v3 codec/chart conditions passed both eyes: 240 scene probes plus16
ordinary/recovery controls, with zero Vulkan/sync errors. Maximum original-source
RGBA error was .0130055; maximum decoded-panel/native error was .0000274777.
Largest guard error was 3.909 codes inside the new ±16 contract.

The unchanged actual v2 carriers at HEVC QP23 /1152 reproduced `[true,false]`:
the right guard missed its old contract by 6.552 codes. The same coupled criterion
returned **failure**, as required by the known-bug negative control. This adds30
negative scene probes and two ordinary/recovery controls. Across the nine
conditions, 11799 C++ checks executed. The initial fixture compilation refusal
(`-Werror=misleading-indentation`, before codecs/GPU) is retained separately.

Private receipts, complete compressed bitstreams, decoded images, source pins
and output rows live under ignored
`build/private-robust-scene-stream-699-20261018/`. All child processes are reaped
before the standard completion cleanup and exact owned-cache removal. The
fixture contacts no device, installs nothing and establishes no new headset,
physical-eye, authentication, codec equivalence or performance result.
