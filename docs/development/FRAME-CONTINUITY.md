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
measured and must not be dismissed as an unrelated WLAN fault.

The game also genuinely replaces its head camera across intro/menu/loading/
scenario transitions; five verified two-eye cohort admissions match those head
replacements with no source-render/material failure. Retaining a destroyed
head or borrowing the previous eye pair would revive the earlier stereo bug.
This release removes identified avoidable withdrawal/polling, but does not
claim uninterrupted loading or packet-loss resilience verified on hardware.
The good live install was read only; updating remains a user-directed setup
action. No timer, unattended restart or diagnostic module was deployed.
