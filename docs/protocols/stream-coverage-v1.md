# Exact coverage certificate v1

The production path carries independently authenticated coverage beside the
**unchanged original HEVC access unit**, through Steam Link itself. It does not
reserve an image region or send camera video back to the PC. Authoritative code:
[`stream_bridge_frame.h/.cpp`](../../bridge/stream_bridge_frame.h),
[`stream_bridge_compression.cpp`](../../bridge/stream_bridge_compression.cpp).
Negotiation and authentication are separate contracts in
[bridge v1](stream-bridge-v1.md) and [auth v1](stream-bridge-auth-v1.md).

## Signed layout

The certificate is 176 header bytes followed by encoded coverage. Integers are
unsigned little-endian. Its dedicated `user_data_unregistered` SEI UUID is
`53 46 4e 50 2d 46 52 41 4d 45 2d 56 31 00 00 01`; it is not the control UUID.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `SFV1` |
| 4 | 1 | Version 1 |
| 5 | 1 | Coverage encoding: None 0, Raw 1, RLE 2, RLE+LZ4 3, optional RLE+Zstd 4, raw+Zstd 5 |
| 6 | 2 | Header length 176 |
| 8 | 16 | Authenticated pair identity |
| 24 | 16 | Current session binding |
| 40 | 8 | Nonzero source frame |
| 48 | 8 | Nonzero encoder generation |
| 56 | 4 | Original access-unit byte count |
| 60 | 4 | Encoded coverage byte count |
| 64 | 2 | Coded width |
| 66 | 2 | Coded height |
| 68 | 4 | Decoded R8 byte count; zero for None |
| 72 | 1 | Optional camera intent: 0 unchanged, 1 active, 2 withdrawn |
| 73 | 7 | Reserved, zero |
| 80 | 32 | SHA256 of the complete original access unit |
| 112 | 32 | SHA256 of the encoded coverage |
| 144 | 32 | Authentication tag over bytes 0–143 |
| 176 | variable | Encoded coverage |

`appendCertifiedFrame` adds a dedicated prefix-SEI NAL before the first VCL,
using that VCL's layer/temporal IDs and proper RBSP escaping. Existing control
SEI, foreign metadata, parameter sets and video bytes remain unchanged.
`inspectCertifiedFrame` authenticates the certificate, reconstructs the original
AU without the owned NAL, checks size/digest and requires VCL content. Only then
is `originalAccessUnitVerified` true. A native slot index or submit count cannot
replace its source-frame identity.

## Coverage and bounds

Raw is one R8 sample per coded pixel. RLE is `(count, value)` byte pairs with
nonzero counts up to 255 and an exact total pixel count. RLE+LZ4 is a uint32LE
expanded-RLE byte length followed by one independent LZ4 block; its output must
be an exact valid RLE stream. Native peers require negotiated capability bit32
before sending encoding 3. Each frame is independent; no previous mask or delta
history is needed. All eight sample bits, including fractional opacity, survive.
The encoder selects the smallest complete Raw/RLE/negotiated-LZ4 representation.

The parser authenticates hashes/tag **before decompression** and validates all
lengths before allocating. Axes are nonzero and at most 8192; decoded coverage
is at most 64 MiB. The complete escaped AU must still fit the stricter 16 MiB
AU budget. Oversized incompressible content is refused, not silently reduced.
Malformed blocks, truncated streams and inconsistent pixel totals leave caller
outputs unchanged. None has no mask and proves video bytes only; it never
grants game-image composition. Negotiated camera-only intent is separate.

## Local handoff is not a second video proof

`serializeCoverageMetadata` sends the signed header and coverage on the pinned
local video/compositor route. It does not include VCL bytes.
`inspectCoverageMetadata` therefore does not prove a complete video AU. The
native adapters must additionally join the previously verified exact AU to
the current decoded image, DMA-BUF kernel identity and revocable capture lease.
The compositor must prove that this same image is actually selected by the
native draw, including eye/extent/UV mapping and GPU resource lifetime.

Since 0.2.10, `inspectCoverageMetadataDecoded` also returns the authenticated
expanded samples and their diagnostic min/max once. These are synchronous
caller-owned transient data, not a retained authorization cache. The renderer
copies them into its existing exact-owned R8 pool. Independent decoder-process
validation remains required. Compression, decompression, file/network work and
shader generation stay off compositor render callbacks.

Missing, stale, foreign or unknown proof never chooses an earlier/latest mask.
With authenticated active recovery intent and a current camera acknowledgment it
emits zero game RGBA; otherwise it retains ordinary opaque output. The decoder handoff, camera grant and
native submit checks remain independent. See the
[continuity record](../development/FRAME-CONTINUITY.md) for publication order,
failure handling, measured runs and hardware limits.

## Optional RLE/Zstd encoding 4

Bit64 must be mutually negotiated before any current explicit-RGBA producer uses encoding4.
The four-byte little-endian prefix is the exact RLE byte count, followed by one
independent Zstandard 1.5.7 frame. Its content size must equal the prefix; no
external dictionary, trailing data, skippable or concatenated frames are allowed.
The RLE uses the existing count/value pairs and reconstructs exactly the signed
coded extent, preserving all 256 transmittance values. Existing binding, MAC,
coverage hash and exact video/frame/eye/selected-image checks are unchanged.

The encoder tries this mode only for RLE up to 1MiB when the existing winning
payload is at least 4096 bytes; the complete new payload must be strictly smaller.
Otherwise the exact old representation wins. The decoder also enforces the 1MiB
RLE ceiling before allocation. Compression is level1, independent per frame,
without temporal prediction or an inter-frame dictionary. One-shot decompression
uses bounded context storage and the descriptor-bounded output allocation.

## Optional raw/Zstd encoding5 and recovery intent

Bit256 permits a uint32LE raw byte count followed by one independent Zstd frame.
It must expand exactly to the descriptor's R8 byte count, at most8 MiB. The same
size/dictionary/concatenation checks apply. The sender attempts level1 only for
large RLE inputs and an LZ4 winner of at least64 KiB; the complete result must
strictly win. Optional codecs apply equally to v1 and v2 explicit-RGBA sources.

Bit128 permits signed byte72 recovery intent:0 retains existing policy during
discovery,1 activates camera-only recovery for a live explicit source,2 withdraws
it. The field is covered by the existing authentication tag. It grants no game
pixels, image association or mask reuse. Only a current camera acknowledgment
allows zero game RGBA while exact source/frame/eye/image proof is absent. Real
local session loss clears policy; decoder generation renewal keeps the camera
warm while requiring fresh image proof and authenticated producer binding.
