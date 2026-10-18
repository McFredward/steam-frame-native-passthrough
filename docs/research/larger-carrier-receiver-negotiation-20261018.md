# Larger Steam Link carrier: receiver negotiation review, 2026-10-18

## Decision

A larger carrier is mathematically sound **if the sender actually encodes the
larger picture**. The inspected Frame receive/decode path does not establish a
receiver setting or capability advertisement that makes the unchanged PC sender
do that. Enlarging a Frame capture buffer, Vulkan image or source XR target is
not such a control. Do not repeat the target-width experiment as a quality fix.

This is a bounded negative finding, not proof that every Steam Link version
has the same limit, or that the Frame hardware cannot decode a larger stream.
No device was contacted, installed package changed, stream captured or runtime
experiment performed for this review. This document implements no transport.

## Inputs and identity limits

The retained Frame files were collected on 2026-10-12, before later runtime
updates. The decisive executable is AArch64 `vrlink`, 2,425,024 bytes,
SHA-256 `31addef0c9553d3770b5570487699e96d70271d4073cd6c03b080fdee6ae2153`.
The retained `vrclient.so` and `vrcompositor` were inspected only for relevant
configuration/interface names. A name alone is not a proved control path.
Private hashes, targeted cross references and bounded function excerpts remain
outside this repository; no Valve binary or instruction listing is distributed.

The older [decoder report](../FRAME-DECODER-20261011.md) identifies a different
`vrlink` build. Its addresses must not be reused for this executable. Similarly,
the retained PC driver is not established byte-identical to the user's current
PC runtime. Its independent encoder cap is corroborating static evidence,
not a universal compatibility statement.

The [actual quality comparison](stream-quality-comparison-20261018.md) records
the important hardware distinction: source XR allocations changed from 4040 to
5050 and 6060; uniform-MR target-width requests/readbacks included 5056 and
6080; the matching decoded allocation remained 3264×6400. Ordinary non-MR used
the different foveated 1152×4608 layout. These are video allocations, not direct
per-eye quality measurements. In particular, 3264 may include alignment beyond
an active 3200 chart; its precise active crop was not established by that log.

## What the retained receiver actually does

The current-build `CaptureSetup` function range is `0x32328..0x32874`.
At `0x3235c` it calls `0x30f78`, which constructs a capture-multiplanar format
query and calls the decoder's `VIDIOC_G_FMT`. It copies the returned format,
reads its width/height for the existing diagnostics, stores those dimensions in
the receiver object, and submits the copied format with `VIDIOC_S_FMT` at
`0x32428`. Capture buffer requests and queries follow.

This establishes the direction of the inspected dataflow:

```text
received coded stream → decoder-reported format → capture allocation/import
```

It does not establish this different direction:

```text
larger receiver allocation → higher PC encoder output
```

The [Linux stateful-decoder contract](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)
likewise separates coded/visible dimensions and capture-buffer dimensions.
Capture setup queries the format parsed from the bytestream; dynamic resolution
changes originate from changed stream parameters. Decoder capabilities may be
queried separately, including per-format frame sizes. The retained path is
consistent with that contract. A larger returned buffer, or decoder scaling,
would not recreate scene samples absent from the encoded picture.

The bounded review also traced device opening and capture initialization, the
`frustumMode` setting read and the server-information wait. It did **not** trace
an outgoing capability field into the PC encoder's selected maximum dimensions.
`frustumMode` is not a proved resolution override and must not be changed as
one. `maxRecommendedResolution` strings in companion runtime files do not prove
video negotiation or encoded capacity. Absence of a useful string is not proof
that no negotiation exists elsewhere.

## Public configuration is not an escape from the encoder fit

Valve's [external-vendor Steam Link integration guide](https://github.com/ValveSoftware/openvr/wiki/Steam-Link-Integration-for-External-Vendors)
documents resource-only PC headset-driver fields for recommended render size
and minimum/maximum stream-format width. It specifies a 1536 upper clamp for
the latter and explains its relationship to streaming foveation. That public
third-party-headset configuration is not evidence of a Frame-side uniform-chart
override, nor of an advertised decoder limit that raises an encoder limit.
Installing such a PC driver would also be a separate integration from this
review's game/mod-plus-Frame constraint.

The independently rechecked retained PC encoder has these branches:

| Layout | Encoded extent from input chart W×H | Selected encoder ceiling |
| --- | --- | --- |
| 1 | 2W×2H | 6400×6400 |
| 2 | W×2H | 3200×6400 |
| Other inspected layouts | W×4H | 3200×12800 |

The request path supplies W=H. Every listed layout therefore fits a square chart
to at most 3200 before codec capability queries can lower it. The geometry fit
reduces both axes proportionally. Different layout packing is not a way to
obtain a larger square chart. Public min/max width settings do not write these
selected ceilings in the inspected driver. See the identity and scope limits
in the [quality comparison](stream-quality-comparison-20261018.md).

Even if a receiver advertised a larger decoder capability, that alone would
not remove this independently selected sender ceiling. No inspected receive
path has been shown to change those fields. This does not rule out an
undiscovered alternate sender path or a later Valve-supported format.

## Consequence for the proposed extra mask border

Keeping D×D useful color while adding coverage needs a carrier with enough
additional *delivered* capacity. In the existing v3 packing the useful color
fraction is 2048/3168 per axis. Thus preserving D=4040 requires a chart of at
least 6249.375 pixels per axis, or 6272 if rounded upward to 64-pixel alignment.
Under layout 2 that would require a 6272×12544 encoded picture. Increasing only
the source allocation still lets the retained encoder fit it back down.

This arithmetic applies to current v3, not to every possible compact mask
layout. It is not an assertion that 6272 is supported by any device. A new
packing can reduce overhead or trade mask detail for color, but a larger local
canvas cannot bypass a fixed encoded sample budget. The
[app-only receiver feasibility review](app-only-receiver-feasibility-20261018.md)
separately covers the conditional separate-mask route; it is not implemented
or made safe merely by a larger capture allocation.

## What remains necessary before a receiver-only negotiation change

The missing decisive evidence is one exact chain: a client-advertised codec or
format field, its receive/dispatch point in the unchanged PC sender, the code
that selects a larger encoder extent because of that value, and the matching
actual decoded active geometry. A settings receipt is not that evidence.
Without the sender-side branch, patching the advertisement would be guesswork.

If such a branch is identified in a supported version, it still needs codec
profile/level, dimensions, luma throughput and buffer-count capability checks;
format alignment and plane strides; imported-image limits and memory lifetime;
and measured decode latency/bandwidth. The native eye selectors, active crop,
R0/R1 reconstruction, foveation, projection and reprojection must remain valid
for both eyes across the transition. These are not implied by nominal panel
resolution, a Vulkan maximum image dimension, or one successful allocation.

Until that missing branch is established, do not install a receiver patch that
rewrites format dimensions, selects an arbitrary layout or changes frusta to
simulate more capacity. Keep the working stereo/edge path and investigate a
route that preserves useful RGB within demonstrated transport capacity, rather
than presenting another larger width request as a completed sharpness fix.

## Review scope

This was static inspection of retained inputs and current public primary
documentation. No test/build cache or executable was generated; no automated
runtime test is reported. The private evidence pins the examined files and
exact cross references. It deliberately does not claim an exhaustive reverse
engineering of client negotiation, a measured decoder maximum, or a new
headset-quality outcome.
