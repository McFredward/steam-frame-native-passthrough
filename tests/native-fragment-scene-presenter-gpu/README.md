# Scene contribution presenter: private GPU oracle

This focused fixture executes the new scene-atlas fragment generator with a
private, exactly pinned original PS supplied explicitly. No Valve bytes are
checked in. It does not activate a receiver or contact a device.

```sh
python3 -B tests/native-fragment-scene-presenter-gpu/run.py \
  --original /private/distort_ps_achromatic_se2.spv --record /private/result.json
```

An explicit Khronos validation-layer package is required. Both Intel and Lavapipe
ICDs must exist. The runner validates every generated SPIR-V module, enables
synchronization validation, and removes its temporary compilation/shader cache
only after all subprocesses are reaped. `--mode` selects an affected subset;
`--driver intel --sanitizer` builds the new C++ harness with ASan/UBSan.

The2112x2752 owned single-mip SFLOAT atlas is sampled linearly. Color components
are inverse-encoded for the native decoder; G in the four mask tiles is an
independent analytic image, including global halos. The unchanged actual original
PS at independently retargeted color/mask coordinates supplies a second color
oracle and inverse-decoder raw-G oracle. CPU equations independently model native
weighting and coordinate arithmetic. The real source-over pipeline uses
ONE, ONE_MINUS_SRC_ALPHA; it retains positive contribution when A=0.

146 positive/negative cases cover current quarter/full/asymmetric R1, both
metadata selectors, four native flag routes, unequal color/mask weights, partial
aperture, all global tile boundaries/edges, 12 separately corrupted code cells,
inclusive +/-2 and exclusive +/-3 thresholds, malformed/ordinary sources, and
ON/OFF/ON source updates with the same already-recorded command buffer. A second
image has its own invalid-to-valid transition. A copied valid strip plus changed
body deliberately remains recognized: format is not content authentication.

Four SPIR-V-valid mutants must fail the actual numerical GPU oracle: multiply
contribution by coverage, use the wrong lower tile origin, use u instead of the
current-R1 q chart, and share the color weight with the mask reconstruction. A
separate coordinate-output diagnostic checks all45 seams/edges; it does not
replace the actual contribution output proof. Invalid/OFF compares all four
original result words exactly, including deliberately invalid R1 inputs.

Out-of-range raw G (-0.2/1.2), and code255 tolerance endpoints above1, are explicit
SFLOAT mathematical clamp/window controls, not claims about SDR UNorm video.
This proof establishes neither native VS/device/frame/eye association nor the
Unity-to-codec color transfer, filtering, headset output or performance.

## Full-resolution v2 and dual fallback

`--dual-full` executes the final frozen 3168-square windmill profile with 2048-square
coverage, then selected legacy sources and v2/ordinary/v1/v2 transitions in the
same recorded command buffers. The physical Float image is 3168-square in this
mode. The original single-profile 146-case mode above remains separate evidence.
New step and constant.25/.5 cases retain actual output RGBA samples. Only a step's
analytic alpha/source-over tolerance uses the physical device's reported
`subTexelPrecisionBits`; existing smooth/constant/original checks are unchanged.
Two valid SPIR-V mutants remove the rotated-strip map or multiply positive
A0-contribution by coverage, and must fail numerically. No timing or codec/headset
claim follows from this focused mode. See
[the contract and evidence scope](../../docs/research/full-resolution-native-coverage-20261018.md).
