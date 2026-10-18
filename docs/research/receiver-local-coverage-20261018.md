# Receiver-to-compositor coverage transport

This is a worker-only local transport and immediate shared-lease implementation.
No Frame was accessed or modified. It does not install a loader, restart Steam,
change the native shader or claim a completed headset picture. The caller must
integrate both process roles and their GPU lifetime gates.

## The actual process boundary

The current decoder/exporter is `vrlink`, SHA-256
`31addef0c9553d3770b5570487699e96d70271d4073cd6c03b080fdee6ae2153`
(2,425,024 bytes). The current final native presenter is `vrcompositor`, SHA-256
`0bb148eda6b5e54e2fbed09d475a43671f5ff6fcb6c2b4f48345b1a9fe8d6f96`
(6,026,096 bytes). Their Vulkan handles are distinct. A `NativeImages` export
join in the decoder process is not an imported compositor VkImage identity.
The compositor must observe its actual public `VkImportMemoryFdInfoKHR`, retain
the imported allocation identity during cold allocation, and join its own bound
image/view/descriptor. The older differently pinned resource report describes
this native IPC structure; its old private offsets are not reused.

Cold startup is also a real boundary. In the current retained `vrlink`, renderer
initialization `0x35b48` calls codec startup at `0x35b90` (`0x337e8` → `0x33668`
→ `0x33248` → `0x32e38`, including OUTPUT format/pool/mapping), before the next
call at `0x35b98` (`0x349b8` → `0x33f68`, native `vkCreateInstance` at `0x33fdc`).
Therefore Vulkan negotiation alone is too late to observe initial OUTPUT setup.
The checked candidate needs process-scoped early preload before native main.
The release build of the existing tiny C exec/spawn selector can provide that
early boundary through a reversible user Steam-service drop-in. Only the tiny
selector is preloaded in Steam. It adds the heavy library solely to the exact
`/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlink` child. The heavy constructor
must remove its own preload token from actual environ before starting threads;
the later Vulkan loader reuses the same library inode/state. Cold preparation
must be idempotent, and callbacks/library/originals remain loaded (NODELETE).

No stock SteamVR executable or operating-system file is rewritten. The installer
must retain the identities of its owned user files and recovery instructions
before activation. A changed executable pin leaves ordinary native forwarding.
The selector freezes only its private four-line launch.conf once in its cold
constructor; public exec/spawn wrappers perform no configuration I/O. The exact
format is `SFNP_STREAM_LAUNCH/1`, native target path, absolute tiny-selector path,
and absolute heavy-library path, each followed by LF. Its final parent is owned
0700; the regular config must be owned0600, nlink1, bounded and stable, with no
symlink traversal. The release selector preserves foreign loader/diagnostic
variables and adds no LD_AUDIT or probe settings. None of the user-service
installation is performed by these source modules or fixtures.

## Two-stage messages, no copied video

`Channel` uses a private UNIX stream socket. Both ends require actual
`SO_PEERCRED` same-UID/PID and a hash of that peer's `/proc/<pid>/exe`, against
explicit pinned executable bytes. Production producer expects the compositor
pin, and production consumer expects the decoder pin. Tests/version policy may
supply another explicit pin; a network message cannot select it. Unknown files
at the socket path are preserved, including a replacement appearing before
shutdown. No discovery, user interface, service command or restart exists.

The bounded stream protocol has a fixed 192-byte little-endian header and two
queued messages at most. Partial headers/payloads never publish a message;
malformed/extra/truncated SCM_RIGHTS are rejected and received fds closed. Each
tick performs at most eight nonblocking send/receive operations, with at most
64 KiB per operation. Payloads are limited to the provider's 2 MiB observation
limit. Exhaustion refuses new data instead of evicting an older association.

- **Certified:** detached signed frame header plus encoded coverage, never RGB,
  VCL or the original coded AU. It carries exact source session/pool, signed
  frame/encoder identity, extent, binding and native submission facts. This can
  prewarm a GPU upload without granting selected-image permission.
- **Decoded:** the same source-frame identity, exact CAPTURE lease reference,
  and SCM_RIGHTS duplicates of the actual DMA allocation and read-only shared
  lease file. Each copied allocation is `fstat`-checked against that reference
  on both enqueue and receive. Integers from the other process are not local
  resource handles.

The producer must obtain detached bytes only after full authenticated original
AU verification in its actual coded-input hook. The compositor must authenticate
those bytes and compare their descriptor/binding with the local receipt.
Detached verification alone cannot re-establish the absent original AU, and the
transport alone cannot authenticate arbitrary metadata. Its authority comes
from the checked native peer and the separately verified signed frame metadata.
Missing messages or incomplete uploads remain ordinary opaque video.

## Immediate content lifetime

`LeaseWriter` prepares an owned 4 KiB memfd with fixed-size seals and 32 bounded
atomic slots. Consumers receive an actually read-only descriptor, verify its
owner/seals/layout, and map it read-only. The source associates exported fd
kernel identities during resource preparation. Its capture sink performs no
I/O, allocation or lock: the provider serializes every callback under its
existing state lock. Requeue invalidates the corresponding slot **before** the
original QBUF runs, even when the downstream queue fails. Source/capture reset
and provider stop invalidate the appropriate slots synchronously.

Each published reference binds an exact even sequence, capture source/pool/
content/token, index and kernel `(st_dev,st_ino)`. A consumer's `current` performs
only bounded atomic loads. A reused slot, different fd allocation, unfinished
publication, reset or revoked writer cannot pass by using the latest values.
Resource-identity and slot fields use acquire/release atomics, including their
sequence checks. Sequence exhaustion refuses permanently; it never wraps.

The source writer and its sink context must outlive provider stop. Register the
sink after the pinned provider is prepared, before returning from early process
preparation and before any native decoder fd exists. Late registration refuses.
The compositor must keep a mapped `LeaseReader` immutable and alive while draw
callbacks may use it; do not reset/unmap an object concurrently with `current`.
Retire maps on a worker after callback/fence lifetime ends.

Shared scalar validity is still not a substitute for native DMA-BUF GPU
synchronization, immutable mask upload completion or command-submission
lifetime. The final renderer additionally needs its actual descriptor-selected
imported image, current mutually confirmed source, exact eye/UV mapping, upload
fence and checked submission route. A check made only during command recording
cannot undo GPU commands after a later invalidation; the renderer must enforce
its per-submission validity contract.

## Focused evidence

Fixtures execute actual local sockets, peer credentials/executable hashes,
partial stream transfer, SCM_RIGHTS duplication, real fd alias/reuse, malformed
rights cleanup, fixed queue limits and owned-path replacement safety. A separate
forked reader observes source invalidation through the real shared read-only
mapping, without receiving another message; it is explicitly reaped. Provider
fixtures exercise the synchronous sink before original QBUF and actual queried
buffer capacity. These are local software checks, not Frame installation,
receiver/compositor compatibility or a mixed-reality hardware result.
