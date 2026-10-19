# Larger in-band carrier: host sizing audit

Read-only investigation, 2026-10-18. Native documentation baseline `792a11b`.
The permitted PC changes remain limited to the game/mod and public APIs. No
Steam sender, driver, runtime setting or Frame component was changed or loaded.

## Result

**The traced host path cannot admit the proposed larger square RGB/coverage
carrier merely through the public width setting, an application texture extent,
another existing layout or a larger receiver recommendation.** Each associated
layout fits a square input to at most3200. This is a source finding in the exact
retained driver below, consistent with the hardware observations; it is not a
universal headset/decoder limit or proof that every newer driver has this code.
No permitted public hard-limit override was found in this admission chain.

The strategy is mathematically valid if the actual transmitted carrier can
grow. The missing capability is that growth through the unchanged host, not a
larger mod render target or a higher successful settings readback.

## Exact source identity and private receipts

| Property | Value |
| --- | --- |
| Retained input | Supplied Windows `driver_vrlink.dll`, collected2026-10-11 |
| Bytes |13,908,632|
| SHA256 |`1e846be041039798580a91452371f6288c614fb4c152126b137e2c693fb77eef`|
| PE preferred base |`0x180000000`|
| Exact retained SteamVR version |Not established; the earlier collection lacked a version file and PE version resource|
| Current PC runtime |Earlier paired hardware logs identify SteamVR/OpenXR2.17.10; equal current-driver hash is **not** established|

All addresses below are **RVAs in this retained PE**, not stable API addresses
or proposed hook locations. Private source/hash/instruction receipts are under
the main Gloomhaven checkout's ignored
`.planning/debug/steamlink-carrier708/host/`. They include
`static_refs.py`, `setting-references.json`, `layout-callers.json`,
`settings.private-disassembly`, `pin_check.py` and `pin-check-result.json`.
No Valve binary, shader body or disassembly is distributed with this document.

The private `pin_check.py` is reproducible against the retained file and passed
37 checks. It validates that exact hash, decisive instruction/data pins and a
small arithmetic model of the traced fit. It does not execute SteamVR/NVENC or
establish actual encoded dimensions. No full suite or headset test was run.

## Settings to selected layout

| Stage | Source evidence | Consequence |
| --- | --- | --- |
| Public integer access |`IVRSettings_003`, section`driver_vrlink`, requested key names read in the function beginning at`0x9E8450`|These are settings requests, not encoder output readbacks.|
| Foveation predicate |`0x9E54D0`, particularly`0x9E551A`|True only for integer modes0 or2. Labels such as forced/automatic are not inferred.|
| Requested layout |`0xA19054`–`0xA1906D`|False predicate explicitly stores requested layout2 at encoder`+0x18`; true predicate uses virtual slot`+0x58`.|
| Backend preference |NVENC vtable`0xC3D9C8`, slot`+0x58`→`0x9FB300`; AMF vtable`0xC3B128`, same slot→`0xA16730`|NVENC returns preferred layout0; AMF returns1. This is not an alpha or larger-square mode.|
| Connection compatibility |`0x9EBF26`–`0x9EBF4B`|Connection field`+0x7CA8` below4 forces current/requested layout0; otherwise requested layout is admitted. Its complete wire semantics are not named here.|
| Actual target update |`0x9EBF73`–`0x9EBF95`|The same requested width is passed as both inputW and inputH to encoder virtual slot`+0x10`. Both inspected backends point that slot to`0xA19320`.|

The current game helper deliberately requests `foveationMode=1`, so this
predicate chooses nonfoveated requested layout2. It also sets
`automaticStreamFormatWidth=false` and the requested
`nonFoveatedStreamFormatWidth`. The source can be checked in GloomhavenVR's
`tools/steamlink-stream-quality.c`; these are ordinary public settings writes,
not a bypass of the later fit.

An arbitrarily rectangular application render target does not change the
demonstrated W=H target-update arguments. Hypothetical differently shaped
encoder inputs must not be confused with an existing public rectangle setting.

## There are two independent bounds after choosing a target

The target-selection function reads `minStreamFormatWidth` /
`maxStreamFormatWidth` for the foveated predicate, and
`minNonFoveatedStreamFormatWidth` / `maxNonFoveatedStreamFormatWidth` otherwise.
Those values feed default/automatic selection. In a valid explicit manual
request, control jumps past those automatic bounds. An additional clamp at
`0x9E8BF9`–`0x9E8C52` still compares the candidate against encoder field`+0x08`
and writes a reduced setting if necessary.

That width check is not sufficient: the later actual layout fitter independently
enforces **both** full-video axes and changes its W/H arguments proportionally.
Consequently a width setting can be accepted before a later height fit reduces
the effective square input. The deferred target/layout update is not an atomic
encoder-size receipt.

The maxima initializer at`0xA18E50` assigns encoder fields`+0x08` and`+0x0C`
from the current layout. The actual fit at`0xA19320` first limits total width,
then total height, preserving aspect through integer proportional scaling:

| Current layout | Nominal full video from inputW/H | Associated internal maximum | Maximum square input |
| --- | --- | --- | ---: |
|1|2W ×2H|6400 ×6400|3200|
|2|W ×2H|3200 ×6400|3200|
|0/other branch|W ×4H|3200 ×12800|3200|

For square inputC, the associated bound is
`C <= min(maxVideoWidth / widthMultiplier, maxVideoHeight / heightMultiplier)`.
The larger full-video area of another layout belongs to **that layout's own
packing**. It does not provide a freely available larger square plane for the
mod. Existing layout numbers are internal branches, not interchangeable
documented application modes.

The traced NVENC initialization calls the maxima initializer, queries encoder
capabilities0x10/0x11 and replaces each maximum only when the returned positive
value is **smaller**. It contains no raising branch (`0xA1E851`–`0xA1E8B8`).
These are PC encoder capability queries, not a receiver-advertised override.
AMF's corresponding hardware capability handling was not broadened into a
separate audit; its shared fit and preferred-layout identity were checked.

## Why a different recommendation is insufficient

A receiver recommendation, per-headset default or manually enlarged min/max
setting may influence the chosen target. It does not rewrite the immediate
host maxima in the traced path. Changing the connection compatibility field
can select a supported existing packing; it does not assign a larger maximum.
This audit does not prove the absence of every possible setting in every build;
it identifies the specific later operations that any claimed bypass must defeat.

Valve's public [Steam Link vendor guide][vendor] documents resource-only
headset/controller integration and stream width/recommended render settings.
It also warns that render resolution and foveated stream width jointly affect
the dense region. It does not document an application hard-limit override or
an auxiliary alpha plane. `IVRSettings` merely provides typed settings access;
the [public OpenVR header][openvr] does not make an accepted setting value an
encoder-dimension guarantee.

The independent [Steam Frame Advanced Options research][external] uses a
different explicitly identified host hash. Its automatic-width and foveation
notes are consistent in scope, but its modified-host experiments cannot prove
that our unchanged host admits a larger carrier. Its reported2048×8192 stream
is a different packing and is not itself a3200-square-limit counterexample.

## Required size for the proposed v3 carrier

The current logical scene square occupies2048/3168=64/99 of each carrier axis.
Preserving a source gridS without spatial downsampling therefore requires at
least `C = S * 99 / 64`; this is sample-budget arithmetic, not equivalence to
ordinary MR-Off's different foveated density or perceived image quality.

For S=4040, idealC is6249.375, or6272 after upward64-pixel alignment. The existing
host sizing branches would then need at least these associated full-video bounds:

| Layout | Nominal dimensions for C=6272 | Retained associated bound |
| --- | --- | --- |
|1|12544 ×12544|6400 ×6400|
|2|6272 ×12544|3200 ×6400|
|0/other|6272 ×25088|3200 ×12800|

All associated branches reduce that square again. At nominal fittedC=3200,
v3's useful scene grid is approximately2069 samples per axis before later
filtering. Do not calculate from3264 as though the entire decoded allocation
were known active content: its exact current padding/active-chart correspondence
is not established by these host instructions.

Even a genuine larger carrier must preserve the scene's original apparent FOV,
native source pose, stereo and exact color/coverage mapping. Re-enabling normal
foveation is not a free fix: panel locations then traverse the native nonlinear
packing/reconstruction, and uniform-v3 guard/coverage sampling cannot simply be
assumed valid. A supported change of encoder layout alone establishes none of
those registration requirements.

## Decision

Do not ship another requested-width multiplier, switch existing layouts or alter
Frame capability claims as a full-quality repair. In this retained host path,
the change required to raise the internal limits would be a host implementation
change, which is outside the user's boundary. A newer official driver could
change this result only after its actual matching code/negotiation and delivered
dimensions establish a permitted route.

Keep the current production game/build/patch unchanged by this audit. Continue
with the [application-only route decision][routes] if no such supported host
route is established. No additional headset round is required to reproduce the
static source arithmetic above.

[vendor]: https://github.com/ValveSoftware/openvr/wiki/Steam-Link-Integration-for-External-Vendors
[openvr]: https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h
[external]: https://github.com/elliotttate/SteamFrame-Advanced-Options/blob/main/docs/encoder-and-codec-research.md
[routes]: app-only-public-api-feasibility-20261018.md
