# Exact Frame handoff continuity (0.2.2)

This changes the private local video/compositor protocol to SFL2. Authenticated
PC video/coverage certificates are unchanged. It is not an atlas, colour key,
latest-mask substitution, resolution change or timestamp heuristic.

## Evidence and the previous blind spot

The 2026-10-19 paired run (game710, patch0.2.1) has 13,182 admitted PC copies,
bound inputs, immediately-ready readbacks and certified masks. Later staging
loss, transport reconnects and Frame input snapshot loss are absent. Those fixes
worked, but did not establish availability before native draw recording.

The old continuous renderer fixture published certificate and LocalPicture
before every draw and initialized all slots. Its separate telemetry fixture
explicitly accepted that an early ordinary recording stays opaque. A new actual
Renderer/Pool timing case proves the missing contract: 16/16 delayed pictures
left the first native recording unmasked, although their certificate preceded
CAPTURE and their LocalPicture preceded submit. Re-recording recovered the exact
same frame. This RED observation is retained separately, never presented as a
passing continuity test.

## Production ownership order

1. Original successful CAPTURE DQBUF executes exactly once and publishes its
   scalar observation/shared lease under the native provider state mutex.
2. After releasing that mutex, the paired handoff can wait for readiness in the
   video decoder process. The receiver worker remains able to authenticate the
   exact input. Compositor callbacks never wait on decoder/socket/CPU fences.
3. The local compositor receives the authenticated certificate and exact decoded
   DMA_BUF/read-only lease. It publishes and retains the matching scheduled R8
   resource. Picture indexing uses the actual shared lease slot, not sourceFrame
   modulo16; gaps cannot overwrite another still-live native buffer.
4. The pinned reverse Ready receipt carries binding, source, descriptor, input
   content and complete kernel/epoch/sequence/capture lease. The video process
   accepts it only for that same previously queued and still-current lease.
   Only then does the original DQBUF return expose the image to native code.
5. Before native sampling, the original queue owner submits the exact R8 copy
   and transfer-write to fragment-read/layout barrier on the SAME queue.
   prepareSubmit still rejects Pending/Submitting, failed copies, wrong queues
   and stale leases. A successful first copy does not require a CPU fence poll
   before its later ordered GPU read. Fences still govern retirement/reuse.

Signed None needs no GPU publication acknowledgement and preserves ordinary
native output. Unpaired/unsupported streams bypass the handoff entirely. Genuine
local loss, revocation or a100ms emergency deadline preserves original native
return and opaque safety. A timeout is an anomaly, not a successful MR frame.
There is no automatic installation, restart, rollback or routine renegotiation.

The fixed32-slot pool covers native buffers and retained recordings. Storage is
allocated only when used: at1152x4608, R8 image+staging is bounded to about324MiB
before driver alignment. Published pictures, recorded commands and GPU fences
retain their exact storage independently. The local worker drains complete
bounded bursts, advancing its parser between messages; socket/event wakeups
replace serialized2ms sleeps. Old/malformed local protocols are refused.

## Diagnostics and verification limits

Cold counters distinguish opaque/transparent/mixed R8 without image capture.
Renderer progress excludes unrelated native draw count and has bounded changed
five-second snapshots, avoiding the former startup-only diagnostic blind spot.
Native hot callbacks update fixed counters only.

Native handoff lock/errno/lifecycle checks, actual authenticated local IPC and
read-only leases, production Renderer/Pool tests and real Vulkan first-use pixel
sampling establish software properties. They do not prove a correct headset
picture or measured end-to-end latency. Headset verification remains required
for continuous MR, both eyes, immediate toggles, menus and ordinary streaming.

Khronos specifies that the second scope of a barrier outside a render pass
includes later commands in submission order, limited by its destination stage:
[vkCmdPipelineBarrier](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html).
Queue order alone is not used instead of the actual memory/layout barrier.


## Native video reset recovery (0.2.3)

The 0.2.2 paired hardware run on 2026-10-19 first delivers continuous exact
coverage without upload, local-queue or frame-guard refusals. At 13:52:08 the
ordinary receiver reports a frame outside its FEC backtrack window and requests
a native video reset. PC logs confirm encoder reset and renewed transport;
Frame metadata subsequently stops at 5,415 receipts while original video
continues. The exact original reset syscall was not logged by 0.2.2.

The pinned receiver's ordinary reset path stops and restarts OUTPUT without
REQBUFS, QUERYBUF or mmap between those operations. The observer incorrectly
handled STREAMOFF like physical pool destruction: it deleted the still-valid
allocation sizes, offsets and mappings. Every later QBUF became unobservable,
so a renewed authenticated session could never begin. Toggling the game option
could not recreate native allocation events that had already happened.

The fix separates allocation lifetime from queued-picture authority. A stream
stop revokes old observations, source generation and capture leases, and clears
queued/decoded ownership. It retains only established allocation facts. Fresh
successful QBUF operations must prove new ownership, and exact certificates plus
full mutual admission are still required. STREAMON alone grants no authority.
Format/allocation replacement, close and unmap retain their conservative invalidation.
The reset reason is carried as bounded scalar diagnostics to the cold worker;
no logging or transport work is added to native decoder callbacks.

The regression fixture feeds actual DecodeProvider OUTPUT and CAPTURE operations
into the authenticated ReceiverWorker, confirms a session, performs the native
stop/start sequence without remapping, rejects the old lease, and confirms a
fresh session followed by steady exact pictures. Its previous-code failure is
retained separately. Earlier fixtures manually fabricated a fresh source pool
or fully reallocated buffers, so they could not detect this lost allocation
history. Software success does not establish the new headset outcome.

Public semantics: [V4L2 STREAMON/STREAMOFF](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/vidioc-streamon.html)
and the [stateful decoder interface](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html).

## Ahead-of-display camera withdrawal and cold sender polling (0.2.8)

The paired 2026-10-19 run uses game710 and verified patch0.2.7, compositor
PID7151 from19:02:50. Mask uploads have zero refusals and the32-slot pool
returns Ready storage normally. This is not the fixed Pending-pool stall.
At19:07:55 two signed None certificates coincide with five new opaque submits;
at19:08:25 one coincides with four. Camera-client health failures are absent.

Previously the receiver drove the global camera switch from each certificate,
before its corresponding decoded image was necessarily being displayed. Future
None metadata could therefore revoke the camera grant for still-current active
buffers. Demand now follows authenticated decoded/kernel/lease-validated
pictures. An opaque picture releases the switch only after all still-current
active buffers retire. This preserves camera availability, never substitutes an
old mask, and never makes a None-certified frame transparent. Source/channel
loss still revokes immediately. An obsolete shared-epoch observation is cleared
after revocation so it cannot cancel every subsequent same-source recovery.
Cold bounded counters attribute request changes to active/None/source/channel/
lease causes and count deferred withdrawals.

The sender also fetched its complete native swap inventory under the render
metadata mutex every8ms, even when its generation had not changed. An unavailable
snapshot could revoke preparation for one tick, and the cold reader itself could
collide with native Submit/Present. The unchanged-generation path now reads an
atomic revision before acquiring/copying inventory. Every actual draw retains
its exact registry-epoch, texture, live source and encode-input checks. Once the
kernel-checked application pipe has a latched format permission, its unused
preparation query also stops; EOF/new client still requires fresh preparation.

The actual Process fixture tests future metadata, current buffer retirement,
requeue, genuine epoch loss and recovery. It passes267 checks normally and with
ASan/UBSan; three causal controls fail at their intended assertions. Actual
Windows named-pipe checks cover the stopped cold polling and fresh EOF admission.
No shader, resolution, stereo mapping, native camera helper or game camera
ownership is changed.

### Remaining hardware limits

This capture also contains genuine native FEC failures before decoder and
control-source resets:19:05:34.090 and19:05:48.422 report too many missing
packets to retransmit. Hundreds of following lines repeat those aborts, not
hundreds of independent connection failures. The receiver then reports OUTPUT
STREAMOFF and later capture-format resets. Camera-demand fixes cannot prove the
network/data path repaired; these gaps remain a separate investigation.
The contribution of added exact-coverage payload to those losses is not yet
measured and must not be dismissed as an unrelated WLAN fault. New bounded
`coverage payload` totals distinguish original-video bytes, lossless RLE mask
bytes, complete escaped access-unit bytes and the largest such unit. These are
approximate cold diagnostics, never frame identity or admission. They let the
next ordinary run quantify mask overhead without another headset probe.

The game also genuinely replaces its head camera across intro/menu/loading/
scenario transitions; five verified two-eye cohort admissions match those head
replacements with no source-render/material failure. Retaining a destroyed
head or borrowing the previous eye pair would revive the earlier stereo bug.
This release removes identified avoidable withdrawal/polling, but does not
claim uninterrupted loading or packet-loss resilience verified on hardware.
The good live install was read only; updating remains a user-directed setup
action. No timer, unattended restart or diagnostic module was deployed.


## Lossless mask bandwidth and classified source gaps (0.2.9)

The matched 2026-10-19 game710/patch0.2.8 capture contains 21,295 masked
frames, 3,297,085,392 original video bytes, 3,581,818,604 RLE coverage bytes
and 6,897,449,108 complete escaped AU bytes. Coverage added 108.6% of the
original coded-video size; the complete stream was about 2.09 times that size.
The largest certified AU was 1,257,066 bytes. A Frame FEC/backtrack failure at
19:27:31.741 matches the PC's reset storm at19:27:32.890–19:27:34.381. Later
PC transport failures also recover. This establishes substantial avoidable
payload pressure, not a proof that every gap is caused by network congestion.

The long signed-None interval at19:29:09–19:30:15 matches the PC dashboard
opening at19:29:09.935 and closing at19:30:16.898. Shorter loading intervals
are source-contract gated. Current evidence does not distinguish a temporarily
missing declaration from a different native loading/dashboard source PID. The
game does not repeatedly withdraw its live RGBA declaration in this capture;
keeping an obsolete camera or guessing foreign-source alpha would not be a
valid fix. New coherent fixed-cost observations distinguish no declaration,
foreign PID, and a genuinely revoked captured generation. Each new cause and
at most eight changed cold snapshots are reported, independently of the older
exhausted generic reporting budget. Quiet COM metadata polling now respects its
five-second deadline rather than falling back to every8ms.

`CapRleLz4` (bit32) negotiates a new lossless per-frame encoding3. Both current
native participants require the capability before admitting the paired route;
optional protocol callers can still negotiate the older intersection. An old
participant is never sent an unsupported compressed mask. Missing or incompatible
pairing preserves ordinary Steam Link and withholds native-MR application permission.

The encoding is uint32LE decoded-RLE length followed by one independent LZ4
block. It reconstructs exactly the existing RLE pairs and full-resolution R8
coverage, including every fractional alpha value. No image/eye extent, video
resolution, crop, quality, mask sample precision or shader mapping changes.
Every frame remains independently decodable; no previous frame is needed.
The selector compares Raw, RLE and negotiated RLE+LZ4 and sends the smallest.
Compression runs on the existing output-processing path after releasing exact
GPU-stage ownership, never in native Draw/Copy or compositor callbacks.

Before decompression, the receiver authenticates the header and compressed
payload hash. Decoded RLE length is bounded by the exact descriptor's pixel
count and the global byte ceiling before allocation. LZ4 must consume a valid
bounded block with precisely the declared output; RLE pairs must cover exactly
the original pixel count. Truncated blocks, malformed lengths/offsets, forged
hashes/MACs and inconsistent pixel totals are refused transactionally.
`third_party/lz4/README.md` records the pinned upstream source; its BSD licence
ships in the setup archive. There is no external runtime LZ4 dependency.

Focused codec fixtures cover real signed AU and detached metadata roundtrips,
fractional pixels, truncations, malformed blocks and deterministic fuzz inputs,
including ASan/UBSan. Native Encode/Lock tests check negotiated versus older
peers and concurrent full-resolution stage ownership. A focused production
Renderer/Pool test verifies that decompressed fractional R8 values are copied
and sampled byte-for-byte on first use; its public-call driver is a software
fixture, not a headset image or real-GPU measurement. Synthetic host bandwidth
benchmarks show compression potential only. New encoded/raw byte totals,
per-format frame counts and bounded encode timing measure the next real run.

This candidate reduces measured bandwidth waste and makes remaining source gaps
attributable. It does not claim that all loading/menu/network gaps are already
eliminated on hardware. The current camera helper, projection shaders, camera
switch lifetime and exact source/frame/eye/lease safety checks remain unchanged.
No diagnostic module, timer or unattended headset restart is installed.


## Activation CPU path and the remaining native reset (0.2.10)

The maintainer's paired game 710 / patch 0.2.9 run reports the best continuous
picture yet, with a brief black interval after enabling MR. The installed
0.2.9 binary and actual LZ4 use were verified read-only. The final PC totals
are 30,902 masked frames, 2,864,100,875 original coded-video bytes and
1,533,654,459 compressed coverage bytes. Coverage is 53.5% of original video,
compared with 108.6% in the preceding capture; these different runs do not
establish a controlled like-for-like benchmark. Frame mask authentication,
GPU upload and exact-mask association refusals remain zero during the initial
active-mask transition. Later exact-picture misses include signed-None/menu/off
intervals; these counters do not establish one missed active-MR frame per miss.

First active masks arrive around Frame 20:23:00. At 20:23:01.254 the ordinary
receiver retries 34 packets for native frame 4027 (219,027 bytes); at
20:23:01.299733 it reports a frame outside its FEC backtrack window, then
late metadata 4032. The PC clock is approximately 1.28s ahead and reports the
matching transport/reset at 20:23:02.574. Opaque submits rise during recovery.
FEC retries also predate MR at 20:22:14/18. These logs establish native video
loss/reset, not that every packet loss is a patch bug or a WLAN fault.

The active path did contain avoidable CPU work: the compositor authenticated
and validated a compressed certificate, discarded the expanded RLE, then its
renderer decompressed that same mask again, expanded R8 and performed a separate
full-image min/max scan. Decoder DQBUF handoff waits for exact publication, so
this work and first-use resource allocation can delay the native decoder's
progress even without reaching the existing emergency 100ms deadline.

The compositor now authenticates, validates and expands the exact mask once.
Its decoded samples and content range are transient, synchronous callback
arguments; they are not cached in retained certificates or used as a later
frame's coverage. RLE min/max is gathered during existing expansion. The
renderer copies the same full-resolution R8 values into its existing pool,
retains the exact certificate/descriptor/lease checks and uses the supplied
range only for content diagnostics. Existing consumers keep their validated
fallback callback. The independent video-process verification remains intact.
No compressed wire encoding, image extent, shader, camera setting or handshake
permission changes in this repair.

Bounded cold handoff telemetry now reports the maximum measured duration and
counts above 11/22/50ms, including time waiting for Process ownership. This
separates shorter stalls from the previous 100ms-timeout-only observation.
Software fixtures verify authentication and transactional failures, fractional
sample equality, single decoded delivery, no duplicate fallback upload and
current exact first-use storage. Hardware confirmation is still required:
removing proven duplicate work does not establish that the native FEC reset
has been eliminated. No stale-mask substitution, transition timer, automatic
headset operation or opaque-safety bypass is introduced.


## Prerelease paired log review (game 713 / patch 0.2.10)

A fresh read-only capture on 2026-10-19 matches game Build 713, PC SteamVR
2.17.10 and the exact installed 0.2.10 Frame layer on SteamVR 2.18.2,
build 1791331621. It contains 8,481 decoded/uploaded masks, no decode/upload
refusals and 10,005 guarded submits without guard refusals. No native backtrack
reset occurs after the first active masks; initial transport resets precede
active MR. The 124 FEC retries are recoverable packet requests, not 124 visible
image failures. These counters cannot establish every headset pixel.

The sender reports 1,564,247,194 original coded-video bytes and 998,551,528
encoded coverage bytes (63.8% additional coverage); complete escaped AU bytes
are 2,568,559,437. All masks use LZ4. Different viewpoint/content makes this
another measured run, not a controlled comparison with 0.2.9. The original
image, mask precision and extent remain unchanged.

The game genuinely tears down its owned head when the native main-menu anchor
is disabled, before finding a replacement anchor. That removes its coverage
source despite the requested MR setting. This source-proven loading/menu gap
must be fixed in the game lifetime, not by borrowing a previous mask or forcing
foreign source alpha. Later deliberate MR off/on and dashboard/game exit are
separate transitions; cumulative opaque totals must not label them all faults.

Cold handoff milestones contain two 100-ms emergency timeouts: one before
active masks, another later in the run. There is no per-event timestamp to
prove their exact visual effect or attribute them to an individual capture.
A completed requeue without a condition-variable notification can delay a waiter
until its deadline without incrementing the timeout counter, because the final
predicate check sees invalidation. It must not be claimed as proof explaining
these two recorded emergency timeouts.

The previous 0.2.9 compositor segfault occurred during the intentional update
stop, before the new 0.2.10 compositor started. Its native teardown stack also
has historical original-state counterparts. It remains an unresolved teardown
observation, not a demonstrated 0.2.10 gameplay crash and not proof either of
patch causation or exoneration. No speculative teardown patch is justified by
that stack alone. Simultaneous local menu/game composition, final sharpness
and future runtime updates still need their own hardware evidence.

## Exact negative handoff completion and requeue wake (0.2.11)

The prerelease review uncovered two avoidable waits in production source. First,
the native sink invalidated a requeued capture synchronously, but the Process
did not register the worker's capture-change callback. A waiter could remain
asleep until the unchanged 100-ms deadline even though its lease was already
invalid. The old fixture masked this by issuing its own notification after the
writer event. The production worker now notifies under the same check/enqueue
mutex. No Process lock or logging is added to the synchronous native sink.

Second, a confirmed peer alone made the decoder wait for every current capture,
including an exact OUTPUT token already permanently quarantined after ordinary
or rejected input. Such a capture cannot produce the authenticated decoded/Ready
receipt. Runtime now reports a separate terminal-negative disposition, only for
that permanently quarantined token in its actual source pool and after current
capture revalidation. Process binds it to the complete current slot, sequence,
epoch, kernel and capture lease. It ends optional waiting without manufacturing
Ready, a mask, camera demand or permission to draw transparently.

A missing or merely delayed certificate is explicitly **not** terminal. It
continues waiting for its real exact proof; unrelated tokens, old leases and
unknown loss cannot use this negative path. Positive publication, camera
acknowledgment, descriptor/eye mapping, authenticated wire and original native
output remain unchanged. `exact-unavailable` is a separate bounded cold counter,
not a successful masked-frame count. No shorter timeout, timer-based install,
resolution change or stale-mask fallback is introduced.

Focused fixtures exercise the actual Process/Runtime, V4L2 observations, signed
AU proof and current lease checks, including delayed valid certificates, known
ordinary input before/after capture, corrupt AU proof, scoped gaps, stale leases,
source/reset loss and unknown loss. Compiling causal controls restore each
defect independently. The notification test now establishes that a real waiter
has entered its condition-variable wait rather than manually repairing it.
Software and sanitizer results accompany the package; a new headset run is
still required. These fixes prove avoidable source waits, not a retrospective
identification of the two emergency timeouts in the previous aggregate logs.
