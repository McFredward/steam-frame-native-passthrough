# Equal-resolution atlas: split-eye failure review

The first hardware run of GloomhavenVR Build 697 with the public dual-profile
receiver produced an unacceptable stereo mismatch: the maintainer reported
unequal apparent image size/depth and passthrough in only one eye. The new v2
format is quarantined from production selection. The previously tested v1
source format remains the recommended path; the hard-edge problem is not claimed
resolved.

## What the existing logs establish

The PC log reports v2 output for both eyes, using 4040-square SDR targets and
8-sample MSAA, across five preparation epochs. The Frame had the intended
31,960-byte dual candidate (`9ad1b56a20db039132ad6ec4557fe8ece3fff277316370bcefc9f70ec7aed367`),
with successful pipeline replacements. Its video log reports a 1152 × 4608 decode
surface. Existing status records contain no per-eye or per-fragment format-guard
verdicts. They therefore do not prove which format branch either eye executed,
or expose the current native R1 values. A previous, separate retained capture
observed R1 `(0, .25, 1, .5)` in that surface layout; this is a parameter for an
offline reproduction, not a new measurement of the failed run.

## Proven failure mechanism

A v2 guard refusal executes the unchanged original shader. For an actual v2
carrier, that means displaying the packed image rather than extracting its
2048-square scene rectangle from the 3168-square canvas. The source scene
coordinate then becomes

```
q_refused = (3168 * q - 560) / 2048
```

instead of `q`. Its slope is 1.546875. For example, output `q=.7` samples scene
`q=.809375`. The original output is opaque. One accepted and one refused eye
therefore produces both symptoms together: different apparent geometry and
camera blending in only one eye. This fallback is appropriate for ordinary
non-atlas video; a fragile atlas recognition code can make it a stereo failure.

A bounded offline surrogate rendered the actual 4040-square guard cells,
bilinearly reduced them to a 1152-square peripheral chart, and used software
H.264/HEVC 4:2:0 with explicit BT.709 limited/full-range conversion. It compared
both frozen profiles at identical encoder settings and backgrounds. This is not
a reproduction of Steam Link's proprietary encoder, active bitrate, QP,
foveation, timing, or physical-eye association.

At HEVC QP 23, the v2 code vector over background 128 passed all twelve
inclusive ±2-code windows. The same encoder settings over background 255
changed cell 4 by about 6.552 codes, so the full v2 signature failed. The two
corresponding v1 controls both passed. Other surrogate cases also show that v1
is not universally immune to lossy coding; this comparison only establishes a
specific regression-sensitive condition.

The measured twelve-value guard vectors were then transplanted into two R1
charts in **one** owned 1152 × 4608 analytic source image. The exact shipping dual
fragment ran on the Intel Vulkan driver, with the private pinned original as a
separate input and the Khronos validation layer enabled. Four focused cases
produced these numerical results; no production decoder was changed:

| Source | Chart / selector | Analytic scene sample | Output coverage |
| --- | --- | --- | --- |
| v2, passing measured vector | R1 y `.25–.5` / 0 | `.700000` | `.5` |
| v2, failing measured vector | R1 y `.75–1` / 1 | `.809373` | `1` |
| v1, first measured vector | R1 y `.25–.5` / 0 | `.700000` | `.5` |
| v1, second measured vector | R1 y `.75–1` / 1 | `.700000` | `.5` |

The failing v2 output matched the original shader's RGBA words exactly. All
40 reproduction checks passed, with zero Vulkan validation errors. This is a
**known-bug reproduction**, not a new passing delivery or a claim that the actual
hardware run used those compressed values. No current headset pixels were read.

## Why the prior checks did not exclude this

The earlier 168-case native GPU proof used a linearly sampled Float atlas. The
fixture painted complete ideal guards back into each selected R1 chart. It
verified correct rotation, halo mapping, composition, explicit format refusal,
and exact original fallback. Its intentional ±3-code cases actually require the
fallback that causes the stereo mismatch when only one real eye fails.

The source render proof checked ideal per-eye output. The additional codec
supplement used lossless 4:2:0, not lossy stream encoding or the smaller peripheral
video chart. Neither proof exercised asymmetric guard damage and then judged
both resulting eye images together. Thus they established useful local
contracts, but not the robustness needed to ship the new packing format.

## Containment and remaining work

Production source selection returns to the retained v1 shader without removing
v2's isolated assets or evidence. The dual receiver still supports that v1 path;
this containment needs no receiver code or device restart. No guard tolerance
was widened, no window classification was introduced, and no timers were added.

Before v2 can return, format recognition must survive the actual transport and
its failure mode must be evaluated as stereo output, including non-atlas video,
asymmetric guard damage, foveated charts, and transitions. A new broad tolerance
or source-only GPU pass does not establish that. If current-run causality must
be distinguished from metadata/chart loss, existing logs are insufficient; a
separately approved bounded measurement would have to record the relevant
current image/chart verdicts. No such new device measurement is authorized or
claimed by this review.

Private source pins, encoder arguments, measured code vectors, four GPU outputs,
generated corner crops and the initial missing-validation-layer refusal are
retained under `build/private-eye-divergence-698-20261018/offline-repro/`. Compiler,
codec and unpacked dependency caches were removed after their processes exited.
