# Demeo mask content and lossless entropy codec

The Battlemarked menu has materially more exact-coverage traffic than the
scenario tested on 2026-10-22. In the supplied PC capture, menu samples reserve
about 204 Mbit/s of metadata on median, versus about 110 Mbit/s in the scenario.
The existing v2 wire-budget policy consequently leaves less space for native
video. This explains a plausible source of pixelation without changing native
foveation; it is not a measurement of the headset image itself.

## Source audit and visual constraint

A private audit of the supplied Battlemarked Unity assets counted 581 materials
and 229 shaders. Twenty-eight materials use additive RGB and One/One additive
alpha: 22 resolve to URP Particles/Unlit, five to Smoke01 Shader Graph and one to
Rainbow01 Shader Graph. Such alpha is intentional real-world occlusion for the
maintainer. After the explicit clarification, the alpha-preservation prototype
was withdrawn before any commit/deployment; its build outputs were removed.
Demeo source and deployment DLLs remain at 1f9f61b. No RGB, glow, lighting,
material blend state, coverage value or menu-placement code changes here.
The audit did not prove an unintended source-alpha defect that can safely be
removed while retaining the requested appearance.

## Optional representation, independent of appearance

PC 0.2.20 and Frame 0.2.16 offer capability64 (RLE/Zstd). Both still require the
original63. Only the mutually negotiated intersection permits the new codec,
and only a current v2 source with the metadata-budget lease opts into it.
A v1/Gloomhaven source retains its previous selector and complete coverage bytes,
even with two new peers. An old peer receives only the old negotiated encoding.

Zstandard 1.5.7 is vendored from upstream commit
f8745da6ff1ad1e7bab384bd1f9d742439278e99; provenance and file digests live in
third_party/zstd/SOURCE.json. The pristine single-file C amalgamation is privately
linked with the BSD license included in the installer archive. Neither injected
library exports the codec or depends on a system Zstd library.

The existing raw/RLE/LZ4 winner is computed first. An independent level1 Zstd
frame is attempted only when the RLE is at most 1MiB and the current winner is
at least4096 bytes. Its complete payload, including the length prefix, must be
strictly smaller. Otherwise the exact old bytes win. This bounds additional
work on large noisy masks; no dictionary, previous mask, downsampling or alpha
quantization is used. The receiver rejects oversized decoded lengths before
allocation and rejects trailing, concatenated and dictionary-dependent frames.
MAC/hash, source/frame/eye and native image-selection proof remain required.

## Diagnostic evidence and limits

Dev Debug logging samples at most32768 existing mask values every128 eligible
encodes. The cold reporter emits at most32 changing statistical samples at the
existing cadence: source/frame identity, encoded/RLE size, fractional-value count,
distinct values and horizontal/vertical changes. These statistics never grant
rendering permission or replace any coverage. Images and RGB are not exported.
Main Important logging does not produce these samples.

The seven synthetic 1152x4608 cases are performance controls, not captured menu
masks. Structured soft edges and sparse fractional particles used about38% and
29% fewer encoded bytes in the local experiment; already-small and oversized
noisy cases retained the old representation. Exact round trips preserve every
original byte. Authentication corruption performs no decompression; the combined
receiver expands once. Malformed, truncated, concatenated and excessive-length
blocks are covered by sanitizer checks. A fresh headset run is still required to
measure real gains and latency in each game. The software cannot promise useful
video quality at a fixed bitrate smaller than the mandatory exact mask traffic.

This is a real two-sided component update. The user-run installer may update the
Frame decoder for 0.2.16. Its existing PC-only branch still leaves an unchanged
Frame untouched and leaves PC SteamVR closed. No device was installed/restarted
during software validation. User-facing READMEs remain brief and unchanged.
