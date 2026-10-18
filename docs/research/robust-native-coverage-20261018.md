# Robust full-resolution coverage, 2026-10-18

## Goal and bounded correction

The maintainer requests the full-resolution opacity field after the failed v2
stereo test. The [v2 regression review](eye-divergence-20261018.md) remains the
record of that hardware report. The real failing frame's guard values, encoder
parameters and R1 chart were not recorded; do not replace that missing evidence
with a claim that an owned codec is Steam Link's actual encoder.

V3 retains the exact2048-square RGB and coverage,3168-square canvas, windmill
tiles,16-pixel global halos and contribution/transfer semantics. Twelve larger
128-square neutral cells occupy two unused corners. Four separated levels and
inclusive ±16-code recognition windows replace v2's small32-square/±2 header.
The [frozen contract](../protocols/scene-contribution-atlas-v3.json) and
[developer guide](../DEVELOPING.md#equal-resolution-v3) define the exact layout.
Coverage itself is never thresholded, reconstructed through the old C1 filter,
or inferred from scene RGB. Genuine fractional opacity and E>0 at A0 remain valid.

The shipping candidate is31384 bytes, SHA256
`44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49`.
It recognizes v3 first, then complete frozen v1, otherwise exact native output.
The accepted-v3/legacy-v1/ordinary-first-mismatch guard budgets remain12/13/2.
No extra descriptor, image, camera, eye queue, cached verdict, native R1 mapping
or PC sender modification is introduced. Historical `transform_dual` still
generates the exact31960-byte v2/v1 executable9ad1b56a…; v2 is quarantined.
All ten original runtime fingerprints remain unchanged.

The new header identifies a format, not authenticated content or exact atlas
registration. A small whole-image translation can leave all sample centers
inside the flat cells. Same-current-invocation image/metadata binding remains
essential and is not established by a passing signature alone.

## Evidence that closes the old test gap

GloomhavenVR's actual compiled Unity2021.3.5 bank is181740 bytes, SHA256
`cb9fc34466df92c64f53f10f5ef3a64b9168de8933d5aaa0b3cf8c61cc6e5bf0`.
Two independent4040-square sources with actual8xMSAA pass227 producer checks:
24 format cells and30 source/scene/mask probes have zero measured error.
All18 retained compiled shaders are unchanged, and the complete v2/v3 carrier
pictures are byte-identical outside the union of their different headers.

The new `native-fragment-robust-scene-stream` fixture uses those complete actual
PNGs, not freshly painted ideal headers. It downsamples to1152 or576-square eye
charts, performs explicit BT709 limited-range lossy H264/HEVC QP23/32 conversion,
then reconstructs the original encoded-channel convention from the complete
decoded RGB raster. The actual native fragment runs on both quarter charts in
one1152×4608 or576×2304 Float texture.

All eight v3 conditions preserve both eyes at30 source positions each, including
fractional glass, additive A0 and strip seams. Original native GPU panel draws
and independent source/source-over oracles check the result. Maximum guard error
is3.909 codes, maximum source RGBA error0.013006, and maximum output versus the
decoded-raster oracle error0.0000275. Same-command ordinary OFF/recovery controls
remain bit-identical to original output. Vulkan/synchronization validation has
zero errors.

The required old-v2 control uses the same actual producers with HEVC QP23. It
reproduces accepted-left/refused-right, with a6.552-code right header error. The
valid-stream stereo criterion fails this pair. Exact fallback is retained for
safety, but is no longer mislabeled a valid-stream quality success. The initial
fixture serializer compile failure happened before codec/GPU work; its refusal
and the narrow correction are retained separately.

This is still an owned chart/codec/channel-reconstruction surrogate. It does not
measure current physical-eye association, proprietary foveation/codec behavior,
real late-frame metadata, Frame performance or headset appearance. Larger masks
also trade video area away from RGB at a fixed carrier size. Real stereo
agreement and the desired window-edge appearance require the next hardware test.

## Additional focused checks and delivery

Seven new profile/guard/CLI methods, historical profile controls,23 package
methods and13 session methods pass; one existing platform skip remains recorded.
Native admission passes nine checks normally and with sanitizers. A separate
measured-guard-vector GPU replay passes77 checks/twelve eye cases; it is not a
substitute for the whole-picture fixture above. Real host and AArch64 release
libraries compile with the production release switches and the exact new pin.
Three focused installer methods and two stopped-upgrade preference cases preserve
enabled/disabled without automatic restart or camera activation. Their controlled
unit/runtime-verifier boundaries are explicit; no real device update is claimed.

Source, logs, failed/corrected fixture evidence, private original/candidates,
codec outputs and receipts stay below ignored `build/private-robust*` and
`build/private-native-robust699-20261018`. They are not public Valve artifacts.
Disposable compiler/GPU/codec caches are removed after child processes are reaped.

Both game producer and receiver must update together. An old receiver cannot
decode v3 and may display the packed picture. No Frame file, camera setting,
installation or service was changed during offline preparation. Deployment needs
a fresh agreed update/restart with retained backup and manual rollback; there
is no automatic rollback timer. No new headset success is claimed here.
