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
