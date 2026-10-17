# Full-resolution scene coverage receiver

The frozen [v2 atlas](../protocols/scene-contribution-atlas-v2.json) carries a
2048×2048 contribution image and a separate 2048×2048 numeric coverage image in a
3168×3168 carrier. This replaces v1's 1024×1024 coverage grid without adding a
window classification, threshold, extra image, descriptor, or frame queue. It
changes the image packing contract; it does not increase the transport's actual
pixel dimensions. Video filtering/compression and ordinary source antialiasing
still apply.

The scene rectangle is `[560,560,2048,2048]`. Four 512-row coverage strips form a
windmill around it. Their data origins are `[576,32]`, `[2624,576]`, `[544,2624]`,
`[32,544]`. Strips 1 and 3 are rotated: the inverse mapping in texel-center index
coordinates is `(origin.x + localY, origin.y + 2047 - globalX)`. Strips 0 and 2
use `(origin.x + globalX, origin.y + localY)`. Every 16-pixel halo continues the
**global** coverage image; only global image boundaries clamp. The offsets keep
the corner halos disjoint. The earlier overlapping draft is not admitted.

The canonical JSON SHA-256 is
`68ad7a14b094729faffb3ff23cc5b42b2605ae9535e28d55071cd76533aecfc5`.
The twelve new guard cells are diagonal at `(64+36i,64+36i)`, size 32, with codes
`104,14,149,250,167,249,185,25,121,144,38,152`, inclusive tolerance 2. The complete
signature recognizes a format, not source authenticity, liveness, or a physical
eye. Color and coverage use the same original image, native invocation R1 chart,
and existing sampler. No CPU eye table or latest-mask join is added.

`transform_dual()` recognizes the complete v2 guard first. If it fails, it runs
the complete legacy v1 guard and body. If both fail, or original geometry is
invalid, it stores the unchanged original SSA994. The original function prefix,
native samples, reprojection, aperture and tint remain intact. Valid v2 needs 12
new guard reads; valid legacy v1 needs 13; an ordinary image whose two first cells
both fail needs 2. Each accepted body adds its existing four panel reads at LOD0.
The old single-profile `transform()` default remains byte-identical.

RGB remains **contribution E**, including positive emission at A=0. Only coverage
is clamped to `[0,1]`; both retain the original aperture and color retains the
original tint. There is no E×A multiplication or edge threshold. The release
preparer generates this dual executable, and its exact bytes/hash are pinned in
the native admission header, session helper and package compatibility manifest.
All ten original runtime-file fingerprints remain unchanged. Historical staging
receipt names remain the same ABI and do not identify which image format a draw
recognized.

## Focused evidence

`tests/test_native_fragment_scene_full_presenter.py` checks the frozen contract,
225 rotated tile/edge coordinates, original function/load/sample preservation,
count1/count3 SPIR-V validation, and byte-identical OFF recovery. Packaging tests
exercise the **actual dual-generation call**, refuse wrong generation/changed
source, and bind both protocol JSON files into the source snapshot. A focused
C++ `--scene-pin` mode admits the exact dual executable and refuses mutation,
cross-profile use and the former single-profile length.

The real Vulkan fixture uses the private pinned original fragment as an input;
no Valve body is published. `--dual-full` adds v2 coverage/rotations, step pixels,
constant fractional coverage, legacy fallback and repeated
v2→ordinary→v1→v2 source changes in the same recorded command buffers. It compares
both a separate analytic sampler oracle and the actual original native shader
retargeted independently to the color/coverage coordinates, then executes real
ONE/ONE_MINUS_SRC_ALPHA composition. Wrong-rotation and E×A mutants must fail the
numerical oracle; the latter explicitly destroys positive E at A=0.

The new discontinuous step controls use the device's reported
`subTexelPrecisionBits` bound for their analytic alpha/source-over comparison.
All inherited smooth, constant, native-original and exact-fallback checks keep
their previous tolerance. The retained actual output samples expose this
precision instead of claiming an infinitely exact continuous edge.

This is focused software/GPU evidence. It is not a new headset, codec, persistent
installation, physical-eye association or performance result. The unchanged
legacy GPU suite is inherited evidence, not a newly rerun complete gate.

## Shipping checks (2026-10-18)

The UNIX CMake registration `native_fragment_scene_full_presenter` was executed
through CTest with the explicit private original input: all four methods passed,
with no skips. This is one focused CTest, not a complete CTest run.

The actual `VkLayer_frame_pixel_snapshot` Release target built for host x86-64
and cross-compiled AArch64 with `BUILD_TESTING=OFF`,
`FRAME_MASK_BUILD_PIXEL_SNAPSHOT=ON`, `FRAME_PIXEL_SYNTHETIC_PRESENTER=ON` and
`FRAME_PIXEL_RELEASE_RECEIVER=ON`. Both linked with `-Werror`, contained exactly
one release marker and the new dual candidate pin, and had no `libvulkan`
dependency. The retained libraries are 182256 and 227328 bytes respectively.
This cross-build did not bypass or run the preparer's native AArch64 guard and
is not installation or native-device execution evidence.

Three existing focused installer controls were rerun for stopped upgrade,
live-update refusal/interrupted recovery, and automatic preparation/reuse. An
owned filesystem delta also replaced the former v1 candidate/compatibility with
the actual dual candidate/new compatibility and library, exercising both saved
enabled values. Production installation transactions preserved each preference;
unit management and runtime verification were explicit fixture boundaries. The
real source-checkout verifier rejects the old compatibility pin, causing the
normal cold rebuild before stopped replacement; old receipt hashes establish
file ownership rather than admitting old executable code as the new package.
Small source pins, libraries, logs and receipts are retained privately; all
compiler/CMake caches were removed after their processes were reaped. Earlier
168-case GPU, ten Python-method and eight C++-check evidence is reused for
unchanged areas, without another full-suite or hardware claim.
