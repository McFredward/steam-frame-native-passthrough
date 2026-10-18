# Sender bridge: receiver boundary and integration status

## Integrated candidate update

The later development tree now connects the actual PC source/encoder observer,
authenticated full-AU certificate, early native decoder provider, checked local
DMA/lease handoff and an independent R8 native-presenter companion. The older
foundation analysis below records the original missing ports; it is historical,
not a statement that the integrated sources are still only a policy model.

The full-colour route does not reserve an RGB atlas or change the game's eye
resolution. The signed, losslessly encoded mask travels in the same native HEVC
access unit and is matched by the actual native token/content and imported
kernel allocation. Both original foveated UV contributions remain intact.
Missing authority preserves opaque original RGB. Full-frame and native decoder
byte budgets are admitted before any augmented video is submitted.

Native command buffers use ordinary flags=0. Immutable GPU control commands
revalidate every submission, including legal reuse, instead of modifying a UBO
that a previous GPU submission might still read. The mask pool retains actual
upload/submission fences; previously initialized images may schedule their next
copy before the same queue draws, without a CPU fence wait. Descriptor/view and
image generations are checked again at submission. Per-eye source admission and
one whole-frame validity decision prevent a guessed left/right association.

The explicit preview installer stages only owned user files, suspends its old
activation transactionally, keeps per-key recovery receipts, and does not restart
Steam or roll back on a timer. After the user starts Steam/SteamVR, it performs a
fresh read-only compositor/UI startup check and asks whether the picture is
usable. A failed or uncertain start, or a bad picture, offers explicit Undo.
A frozen launch selector requires Steam as well as SteamVR stopped before final
removal. Windows is the first real sender candidate; a Linux shell entry does
not imply that a Linux encoder/source adapter has been implemented.

Software evidence includes real D3D11 coverage, real Vulkan pixel/fence tests,
actual production renderer forwarding/lifetime fixtures, authenticated TCP/local
socket/SCM_RIGHTS/shared-memory tests, both platform links and local shader
construction. These do not establish the new combined picture on a headset.
The first paired full-resolution hardware result is still pending. No device
files or services were changed during this integration.


## Scope

This work introduces a bounded, caller-driven receiver foundation for tiny
sender metadata. It is not an installed Steam Link hook, a mask transport or a
new renderer. A valid handshake never authorizes a compositor change. Normal
video must keep its original bytes and native decode/presentation behavior when
the sender is absent, incompatible, incomplete or unverifiable.

No Frame was accessed or modified for this work. The retained inputs below are
private copies; their existence does not prove that a currently installed Frame
has identical files. No proprietary binary, instruction listing or shader is
distributed in this repository.

## Concrete coded-input boundary

The retained 2026-10-12 AArch64 `vrlink` has 2,425,024 bytes and SHA-256
`31addef0c9553d3770b5570487699e96d70271d4073cd6c03b080fdee6ae2153`.
Its decoder submission function is the range `0x31cc0..0x32018` and a relative
relocation at `0x248c80` targets its entry. These are coordinates for this one
file, not portable hook addresses or a recovered public C++ interface.

The inspected function retains its incoming pointer-holder, byte count and
fourth argument independently. It reads the holder's buffer pointer and finds
the already acquired decoder buffer by equality with the native pointer table.
It checks that the resulting index matches the key-indexed expected slot and
that the buffer is acquired before queuing it. The supplied byte count becomes
the output plane's bytes-used field. The fourth argument separately becomes
the V4L2 timestamp token before the output-queue ioctl at `0x31e08`.

For this function the observed machine arguments are `x0` (native object),
`x1` (pointer-holder), `w2` (used bytes), `w3` (native key) and the low byte of
`w4` (a native control flag). The object/pointer-holder C++ types and portable
ownership ABI are not recovered. The submission code consumes the holder after
the successful handoff; retaining its pointer for asynchronous parsing would be
unsafe without a separately owned copy or explicit native buffer lease.

Thus there is a real boundary where coded buffer ownership, byte extent and
native key meet before decode. The same key must remain distinct from a reused
buffer index. This current-build finding must not be confused with the older
addresses in the [2026-10-11 association report](../FRAME-ASSOCIATION-DATAFLOW.md).
The retained input does not by itself prove that each invocation is exactly one
complete Annex-B HEVC access unit; the adapter only accepts that framing after
the caller establishes it. It does not infer completeness from a start code,
buffer capacity or native key.

## Concrete provider candidate and missing implementation

The existing receiver observer is loaded through a scoped user-service preload
and interposes version-pinned libc mutex calls. Its exact return-site checks
observe selection/metadata scalars under the corresponding native locks.
It does **not** expose coded buffer bytes, a source session, or the complete
sender-to-selected-picture association. Calling its scalar observation a video
adapter would be incorrect.

A narrowly reviewable coded-input provider could interpose the public V4L2
`ioctl` boundary, admit only the exact pinned caller and output `QBUF` operation,
and resolve bytes through previously tracked `QUERYBUF`/`mmap` ownership. It must
verify the actual decoder fd, queue/memory type, plane count, bytes-used, mapped
extent, acquisition/lifetime and generation, and forward the original call with
the exact result and `errno`. It must not inspect arbitrary pointers obtained
from a guessed private object. Capture-queue calls, other callers and unknown
memory shapes must simply use their original forwarding path.

That provider is **not implemented here**. The current observation library has
neither the required mapped-buffer registry nor an audited versioned forwarding
contract for the variadic ioctl edge. A direct decoder vtable detour would
instead need a separately reviewed loaded-image/lifetime/ABI patch; a discovered
relative relocation is not permission to mutate it. The stream session/reset
boundary and HEVC framing also need a precise source-derived contract.

The retained executable imports `ioctl@GLIBC_2.17`; its decoder entry is not an
exported `SubmitFrameForDecode` function that can simply be resolved by name.
The retained libc exports `ioctl@@GLIBC_2.17`, but the inspection did not establish
an independent `__ioctl` forwarding alias analogous to the existing observer's
`__pthread_mutex_*` aliases. Unknown variadic calls must not be forwarded using
a guessed argument shape. This is a concrete provider/forwarding task, not a
missing public OpenVR initialization call.

The [Linux V4L2 stateful decoder specification](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)
provides the queue/buffer contract, but not Steam Link's private source/session
association. No public API is assumed to expose that missing association.

## Receive, report and display are different gates

The receiver must parse only its owned tiny bridge metadata and leave foreign
messages and original video unchanged. A capability exchange recognizes both
endpoints only after their complete nonce-bound handshake. A reverse control
route is still necessary for the receiver's acknowledgements; receiving a
producer HELLO in a video SEI does not create that reverse transport.

Actual decoded-picture use additionally needs the source-to-encoder-to-native
key association, decode output content generation, imported resource generation
and the exact selected picture, eye and extent. The existing
`FrameAssociationLedger` is a bounded policy implementation for caller-supplied
evidence, not a provider of those facts. Unknown, dropped or conflicting evidence
must not be replaced with a latest-frame, approximate-time or submit-counter
association. A successful metadata lookup is still not a compositor descriptor,
mask image, stereo or reprojection proof.

`receiver/stream_bridge_receiver.{h,cpp}` implements this separation:

- Every AU result returns the exact original pointer and byte length. Parsing
  never strips or modifies the SEI, VCL or foreign bytes, including on exceptions.
- A complete-AU and admitted-source-boundary assertion, authenticated channel,
  session and explicit queue/key generation are required before inspecting bytes.
  These assertions default to false and are caller trust boundaries, not self-
  certification of a loaded native provider.
- Handshake output is separate from confirmed handoff. An unsent COMMIT cannot
  establish recognition. Tiny probe echoes require an actually observed owned
  AU payload; the reverse route itself remains caller-owned and unimplemented.
- A dedicated fixed-capacity metadata join retains at most 32 probes and 64
  surface histories. It does not insert fictitious eye geometry into the mask
  ledger. Selected metadata additionally needs explicit decoded and selected
  association assertions for the same native key/queue generation and exact
  surface namespace, handle and content generation.
- Ordinary surface reuse invalidates an earlier probe association. Late older
  generations, contradictory identities and capacity loss cannot fall back to
  the latest probe. There is no implicit eviction or arrival-time expiry.
- All adapter calls are serialized on a non-render worker. Explicit status
  requests produce copied enum/session notices, at most one per status in a
  session. The caller may forward those to its UI queue. There is no callback
  from a compositor thread, socket, toast loop, sleep or automatic restart.

The shipping scene-atlas presenter and its existing validation remain unchanged.
This foundation performs no renderer activation, image allocation or native
settings change. A missing-sender notification is an explicit once-per-session
status request that an off-render-thread consumer may poll; receiving ordinary
video alone must not cause repeated player messages or block native decode.

## Validation boundary

The actual adapter fixture passes 581 assertions in strict native and
ASan/UBSan builds. It executes the shared wire codec, handshake and HEVC parser
against synthetic complete Annex-B AUs. Three causal controls fail at their
intended assertions when the admitted-provider guard, actual-selection guard or
stale-content guard is removed. This is one focused adapter check, not a repeat
of the complete native gate or a codec-decoding experiment. Private receipts pin
the tested source copies; generated executables were removed after reaping and
the shared completion-cleanup API was invoked.

These checks do not establish a live Steam Link provider, a reverse-channel
delivery, decoded source identity or a headset MR result. No runtime activation
should be advertised until those separate missing links are implemented and
demonstrated.
