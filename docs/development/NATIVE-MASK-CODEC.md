# General native mask compression and camera-only recovery

PC 0.2.20 and Frame 0.2.16 apply these policies to every current explicit-RGBA
producer, including v1/Gloomhaven. Application names and v2 budget leases do not
select a special recovery/codec route. The separate video-rate budget remains
an opt-in v2 source contract; v1 applications retain their existing rate policy.

## Observed failure

The October22 17:15 Battlemarked capture exhausted the native video budget:
menu mask reservations peaked above500 Mbit/s while video reached its1 Mbit/s
floor. Frame logs record repeated FEC retransmissions, a backtrack-window failure
at17:15:39.577, decoder reset and late metadata for frame3894. These are transport
loss observations, not a material alpha or native foveation defect. The tested
patch predates this candidate. Earlier menu captures had lower but still material
metadata traffic; no actual menu-mask image has been exported.

A separate menu-alignment exception in mod1f9 used a broken generated IL2CPP
matrix-return wrapper. Demeo source0060 replaces it with the checked native
Unity6 injected-call ABI. This is separate from transport recovery.

## Appearance remains unchanged

A private audit counted581 Battlemarked materials and229 shaders, including28
additive-alpha materials. The maintainer wants their glow/light to occlude the
real environment. The alpha-edit prototype was withdrawn before deployment.
This change does not alter RGB, lighting, glow, blend state, coverage precision,
mask resolution, native reprojection, distortion or foveation mapping.

## Independent lossless representations

Both peers offer511 and still require63. Optional negotiated bits64,128 and256
permit RLE/Zstd, camera-only recovery and raw/Zstd respectively. Old63 peers keep
their original encodings and behavior. Current source receipts, peer confirmation
and coded-AU roundtrip remain mandatory on the sender.

The old raw/RLE/LZ4 winner is computed first. Level1 RLE/Zstd is attempted only
for RLE at most1 MiB and a winning payload of at least4096 bytes. Level1 raw/Zstd
is attempted only for RLE above1 MiB, raw at most8 MiB and an LZ4 winner of at
least65536 bytes. A complete new payload must be strictly smaller. All256 opacity
values survive; no dictionaries, temporal prediction, quantization or downsampling.
Authentication/hash checks precede bounded decompression. Decoders reject unknown
lengths, dictionaries, trailing/concatenated frames and invalid expanded counts.

Zstandard1.5.7 is vendored from upstream commit
f8745da6ff1ad1e7bab384bd1f9d742439278e99, with exact source digests and BSD license
under third_party/zstd. Both injected libraries link it privately.

Seven synthetic1152x4608 controls are not captured menu masks. Dense247..255
noise used about57% fewer encoded bytes (5.00MB to2.14MB); structured soft edges
and sparse fractional particles saved about38% and29%. Already-small or random
raw cases retained their old representation. On this host the dense case added
about4ms encoding time and reduced decode time; real Frame/game latency remains
to be measured. Byte savings alone cannot prove that network overload is solved.

## Camera-only recovery is policy, never image proof

Signed certificate byte72 carries0 unchanged/discovery,1 active explicit-MR intent,
2 source withdrawn. Bit128 must be negotiated to send nonzero intent. Any current
explicit-RGBA producer may activate it. A decoder generation change preserves the
acknowledged camera intent while revoking all old image/mask authority. Renewed
source generations still require fresh authenticated producer bindings. Explicit
withdrawal or a genuine local connection/consumer loss revokes recovery normally.

The coverage fragment retains its exact native color/mapping for valid game
frames. If current game proof is absent, an active intent plus a current camera
acknowledgment selects zero RGBA, revealing only the real camera. A separately
locally generated companion fragment has no mask descriptor use and handles
missing images or render-metadata contention without guessing a spatial mask.
No Valve shader binary is distributed. Both derivatives are generated from the
locally validated native shader and pinned in the owned installation receipt.

Immutable GPU control commands select ordinary output or recovery afresh on every
native queue submission. A reused recording cannot retain a successful old guard.
If an owned video command in a batch loses proof, all owned exact guards in that
batch become camera-only together. Fresh exact data resumes normal game output.
Render callbacks perform no I/O, decompression, compilation, allocation or waits.
This cannot control headset firmware/camera availability, runtime process death
or submissions outside the admitted native video pipeline.

## Evidence and delivery

Production process tests exercise real authenticated local sockets, source renewal,
camera-helper acknowledgment and exact lease retirement. Production Renderer/Pool
tests execute recorded GPU controls, including submit/Submit2 and retained command
buffers. Independent real Vulkan offscreen tests compare normal RGB bit-for-bit
against the unchanged original and verify zero RGBA during recovery, including
the standalone fragment with set1 unbound. This is software evidence; the owned
fullscreen vertex fixture does not prove the real headset's selected image.

Codec ASAN/UBSAN, malformed input, source/frame/eye proof, installer rollback,
PC-only updates and packaging controls remain required. Dev Debug statistics are
bounded; main Important excludes mask-content samples. The user-run installer
must update both changed components; unchanged Frame installations in future
PC-only updates remain untouched, and PC SteamVR remains closed. No device write
or restart is performed during development validation. A headset test is pending.

The final local validation passed all180 full-project CTests,32 bridge-only
CTests, three codec ASAN/UBSAN tests,345 process checks (also sanitized),906531
Renderer/Pool checks (also sanitized),28 causal controls and840 real Vulkan GPU
checks with zero validation errors. The stereo/contended-recovery subset passed
128091 checks in normal and sanitized runs. Installer91, PC-only21 and packaging15
tests passed. Native optional-codec and shader/source proof stayed unchanged in
the final test-runner routing corrections. Hosted codec, stage, readback, source,
resource, update and lifecycle workflows passed on sourcea4ad13f; the broad hosted
native workflow stalled while installing apt dependencies. Its full local180-test
counterpart passed; do not label the hosted workflow successful from that result.
The setup workflow's individual job steps completed successfully while its API
still reported in_progress. Hardware verification remains false.

## October22 regression: wrapper and pre-main worker lifetime

The18:37 capture on PC0.2.20/Frame0.2.16 entered camera-only recovery on the
first exact source. The real Vulkan wrapper still required a mask descriptor
whenever control set2 was present. Its own rejection marked device tracking lost,
which removed the admitted renderer and closed the local receiver channel at
18:37:54. The game later correctly reported no operational companion. This was
an integration bug missed by the isolated Renderer/Pool and offscreen GPU tests.
The new wrapper regression fails on the shipped code, checks indexed/ordinary
native draws with only set2, original pipeline restoration and subsequent exact
set1/set2 recovery. Negative controls reinstate the erroneous mask requirement
and bind a null recovery mask; each must fail its intended assertion. Incomplete
controlled draws still refuse the optional route.

The same capture also records a shutdown SEGV in ReceiverWorker::stop while the
main thread frees receiver state. A real process-entry fixture starts the worker
in constructor priority200, before ordinary TU initialization; the old global
worker/thread ownership reproducibly segfaults. Worker ownership, condition
variable and synchronization now initialize together on first use. An ordered
exit guard joins the worker before deleting it/restoring imports. One bounded
process-lifetime control object remains valid for late native wake callbacks.
The fixture also exercises16 explicit stop/restart cycles and automatic exit.

Frame0.2.17 contains both corrections. PC remains byte-identical at0.2.20; no
codec, coverage precision, native shader output or game-specific route changes.
Installer0.2.21 carries the new Frame artifact. No device write/restart was
performed during investigation; headset verification remains pending.
