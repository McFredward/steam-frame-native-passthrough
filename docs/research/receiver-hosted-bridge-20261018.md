# Source-pinned hosted Frame receiver

This checkpoint connects the prepared public decoder observer to a real hosted
worker and authenticated control runtime. It is not installed and has not run
on the Frame. The Vulkan layer/render integration is a separate caller of these
ports; no callback in this checkpoint grants image/passthrough permission.

## Native publication and lifetime

`publishPinnedDecodeImports` hashes the actual main executable and loaded RX
segment using the already pinned provider. It admits only that retained
`vrlink` (file SHA-256 `31addef0c9553d3770b5570487699e96d70271d4073cd6c03b080fdee6ae2153`).
Four actual writable, non-executable import slots are replaced with scoped
compare/exchange, after original GLIBC_2.17 targets have been captured. The
slots are `mmap` at `0x24e070`, `munmap` at `0x24e110`, `close` at `0x24e8b0`,
and `ioctl` at `0x24ec58`. They follow GNU RELRO; no page protection or code
modification occurs. A failed transaction restores only its still-owned values.
A foreign competing update is never overwritten during rollback.

Call `startPinnedReceiver` explicitly, before decoder creation, from the checked
candidate loader. Default-off does no allocation, file/socket, hook or thread
work. Original call targets and the callback library must remain process-live;
link the candidate shared object with `-Wl,-z,nodelete`. `stopPinnedReceiver`
joins its worker and restores only still-owned imports. No native file, Steam
service, decoder settings or other process is changed by these functions.
Unknown variadic ioctl calls retain the actual AArch64 tail-forwarding entry.
A late attach lacks pool/mmap lifecycle and must not be called supported.

## Exact worker dataflow

Only the admitted successful OUTPUT QBUF produces an owned input copy. The
worker creates fresh OS-random receiver source identity when an actual native
fd/OUTPUT pool generation first appears. It is not derived from TCP, pairing
material, a received frame counter or a guessed compositor submit number.

A completed native payload is still not assumed to be a complete HEVC AU.
`ControlRuntime::verifyAccessUnit` must first validate the paired frame
certificate and exact original-AU length/SHA. For the initial message it also
requires the matching MAC-valid producer HELLO in that exact original video.
Only this successful check sets the worker-local completeness assertion for
`Receiver::observeAccessUnit`. Original native bytes and `Observation` remain
unchanged. Both peers require all known capabilities. Reverse handshake actions
are marked sent only after their actual full authenticated socket send receipt;
a queued message or a TCP connection is not mutual confirmation.

Worker callbacks provide synchronous borrowed certificates and exact native
facts. Retaining a certificate or coverage requires a consumer-owned bounded
copy. A 32-entry in-flight table joins a unique actual native timestamp token to
its certified sourceFrame/encoderGeneration. Duplicate live tokens, capacity,
copy/event loss and transport loss refuse the session rather than choosing a
newest frame. Ordinary/noncertified video never supplies a certified callback.
The actual decoder input is never changed. A blocked source is retried only
when a new actual OUTPUT pool appears; there is no timed reconnect loop.

OUTPUT retirement ends the source session. CAPTURE-only reconfiguration clears
pending image associations while retaining the unchanged OUTPUT source/control
session. Both operations invalidate renderer evidence via explicit callbacks.
The worker polls a fixed number of input copies/events per tick and performs
socket/crypto work off rendering and decode callbacks. The 2 ms wake interval is
a worker scheduling interval, not an automatic feature expiry or restart.

## Public CAPTURE ownership and image identity

For this exact retained binary, the CAPTURE public-call return addresses are:

| Address | Operation |
|---|---|
| `0x3242c` | CAPTURE_MPLANE one-plane format |
| `0x324a8` | DMABUF CAPTURE pool allocation |
| `0x325c8` | CAPTURE slot QUERYBUF |
| `0x310f8` | CAPTURE QBUF, separate index and exported fd |
| `0x32a5c` | Successful CAPTURE DQBUF and returned native timestamp |

Actual successful DQBUF establishes the exact fd/index, source pool, capture
pool and content cycle. Requeue revokes the old content before calling the
original ioctl, including failure/unsupported-shape paths. An in-process consumer
can call `pinnedCaptureLeaseCurrent`: it uses a try-lock and scalar comparisons
only, never I/O. The actual compositor is another process; it requires the
authenticated local transport and immediate shared lease in
[the process-boundary follow-up](receiver-local-coverage-20261018.md).
A worker-cached event alone cannot authorize a resource after native reuse.
Neither scalar check replaces native Vulkan synchronization/fences.

`NativeImages` independently records public successful external image creation,
DMA-BUF allocation, binding and export. The current source-pinned cold export
return address is `0x34574`: it follows the actual
`VkMemoryGetFdInfoKHR` (`1000074002`) call with fd output at native record+24.
The value of a private wrapper at record+32 is **never** treated as a VkImage.
The actual public DMA_BUF export is duplicated/statted before its original fd
is returned to native. The owned duplicate prevents allocation inode reuse.
Worker `join` compares the CAPTURE fd's current `(st_dev,st_ino)` with that
retained allocation and requires one unique live public bound image, matching
allocation/image generations and supported offset zero. Same fd number alone
is never sufficient. Multiple aliased images, wrong handle type, missing
preparation, close/reuse, free/destroy and unknown shapes refuse the join.

Only DMA-BUF images/memories consume the 64-entry exporter registry. These are
the decoder's own handles; the compositor needs its separate public imported
allocation/image join. A consumer must additionally prove the exact
public descriptor-bound selected VkImage, source certificate, eye/layout/UV,
coverage extent, current mutual confirmation and GPU lifetime. A live VkImage
allocation, frame certificate or tiny metadata handshake alone is insufficient.

## Focused software verification

The provider tests exercise public originals and exact capture lifecycle;
publication tests execute four-slot transactions and competing writers.
Hosted tests use actual selected private credential parsing, protected video
SEI/certificates, full-capability Handshake and nonblocking loopback control,
then source/capture/reset/drop/EOF/capacity paths. Image tests use real OS file
identities, aliases, fd-number reuse and owned duplicates with public Vulkan
handle facts. They are portable controlled fixtures, not a Valve driver or
headset execution. Strict normal/sanitized and AArch64 build results are retained
in private development evidence, alongside source pins and cleanup receipts.
No private Valve code, binaries or instruction bodies are tracked here.
