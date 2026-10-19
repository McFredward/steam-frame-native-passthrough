# Frame decoder association dataflow

> **Historical reference.** Historical native-build investigation. Addresses and observations below belong to the explicitly pinned ELF and are not current installation instructions or a portable ABI. For the installed source/decoder/lease chain, start with [Developing](DEVELOPING.md).

## Finding and correction

The inspected Frame receiver carries a **received 16-bit frame key**, distinct
from its reusable decoder buffer index, into the V4L2 timestamp and back into
metadata lookup. This is stronger evidence for a native frame association than
the previous pointer-table hypothesis in
[the initial decoder report](FRAME-DECODER-20261011.md).

The initial report's statement that the pointer-table index becomes
`timestamp.tv_sec` is incorrect. In function `0x31118`, the fourth argument is
saved at `0x31128`. The pointer lookup separately finds a buffer index. At
`0x31200`, the preserved fourth argument replaces the register previously used
by that lookup. The buffer index is stored in `v4l2_buffer.index` at `0x31244`;
the incoming key is stored in `timestamp.tv_sec` at `0x3124c`, before the queue
ioctl at `0x31260`. These are two different values.

This remains static evidence for one receiver build. It does **not** establish
the PC encoder association, a stream/session epoch, a 64-bit frame identity,
safe resource ownership, or the mask's eye/crop/foveation coordinates. It does
not authorize an executable hook or activation of mask composition.

## Scope

Analysis used the privately supplied, unmodified ELF64 AArch64 `vrlink`:

- SHA-256: `0000009e5c7124bb03bfd444f7200304e0bb3ec1ec0cf80f0086a19ec048132b`.
- ELF build ID: `0a51addb6a9ef05dc52c055b1076947908f20acc`.
- All addresses are ELF virtual addresses for that build, not portable entry points.

Only local, read-only analysis was performed. No process was attached, no Frame
files were changed, and no runtime association or headset result was verified.
Descriptive function names below are analysis labels, not recovered API names.
Private binaries and instruction listings remain outside tracked files.

## Producer-to-decoder chain

The strongest caller evidence traces the token before decoder submission:

| Stage | Evidence | Meaning established |
| --- | --- | --- |
| Received packet | `0x52648` in `0x52628` reads a 16-bit field at packet offset `2`. | The key comes from incoming packet data. |
| Frame record initialization | `0x52804` passes that key to the virtual initializer; the corresponding vtable entry resolves to `0x50d80`. `0x50d98` retains 16 bits and `0x50e98` writes record offset `8`. | A reusable receive record retains the packet key. |
| Completed payload | `0x52920` reads record offset `8`; `0x52980` calls `0x52088` with the key, payload size, and payload-pointer holder. | The completed/reassembled payload and key reach the submission manager together. |
| Decoder submission | `0x520b4` retains the 16-bit key; `0x52204` passes it as the fourth argument. The virtual call at `0x52234` resolves through the decoder vtable to `0x31118`. | The decoder's fourth argument is the received key, not an input slot. |
| V4L2 queue | `0x31128`, `0x31200`, `0x31244`, `0x3124c`. | Buffer index and timestamp token remain separate, as corrected above. |
| Capture dequeue | `0x31ec4` reads the returned timestamp. `0x31f50` supplies its low 32 bits separately from the CAPTURE index; `0x31f5c` calls `0x34308`. | The decoded result carries the queued key independently of its surface slot. |
| Metadata lookup | `0x34334` preserves that token; `0x3439c` calls `0x51fd0` with it. | Decoded capture and stream metadata share the received key. |

This is a packet/FEC frame key in the receive path, not proof of the sender's
public `PresentInfo_t.nFrameId`. No receive-local counter can substitute for
the missing PC association.

The Linux stateful decoder contract supports this mechanism: CAPTURE results
retain the originating OUTPUT timestamp even when decode order differs from
display order. Multiple decoded results can share a timestamp, so a capture
slot/generation is still a separate lifetime concern.
[Linux V4L2 decoder documentation](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html#decoding)

## Slot reuse and metadata lifetime

The receive records use a 16-entry table selected by the key's low four bits.
That is bounded bookkeeping, **not** evidence that the frame key has only 16
values. Metadata lookup `0x51fd0` masks the requested key to 16 bits
(`0x51fec`), requires the record's metadata-valid flag, and compares the full
record key at offset `8` with the requested key (`0x52024`). Missing validity
or unequal keys returns no metadata. A slot collision alone cannot succeed.

Metadata acceptance `0x511a8` copies a `0x154`-byte metadata block from packet
offset `0x28` to record offset `0x4278` (`0x51250`–`0x51260`), then marks it
valid. Its first 32-bit field is used by a separate presentation-number mapping
and a diagnostic labelled `m_iPresentNumber`. That label does not prove its
relationship to an application submit or PC encoder frame number.

The reset path `0x52540` explicitly invalidates the 16 records: stored keys
become `-1`, metadata-valid flags become false, and payload pointers are cleared
at `0x525f8`–`0x52600`. Record initialization also clears metadata validity.
Lookup returns an internal metadata pointer after releasing its lock; this
inspection does not establish a lifetime suitable for an external asynchronous
consumer. Retention or copying must be proven separately.

Decoded records have a separate reuse generation. `0x34308` stores the key and
local timing value at `0x34440`, and the generation at `0x34444`. The selector
`0x345a8` checks records and metadata before choosing a presentation record.
The presentation path calls that selector at `0x34a28`, reads the **chosen
record's** key at `0x34a90`, and looks up its metadata at `0x34aa4`. A mask must
follow this selected video record, including repeated presentation; selecting
the latest decoded frame would discard the native association.

## Remaining bridge requirements

1. The [subsequently supplied PC driver](PC-DRIVER-20261011.md) establishes that
   the sender assigns the 16-bit key separately from queued encoder metadata.
   Prove coverage's composition/resource association through that assignment;
   neither the presentation number nor its low 16 bits can substitute for it.
2. Establish a stream epoch and wrap/restart rules. A 16-bit key cannot directly
   serve as this fork's session-scoped 64-bit mask frame identity. Do not infer
   wrap generation from arrival order or truncate the mask identity.
3. Establish the selected surface's resource lifetime, synchronization fences,
   and compositor descriptor binding without guessing a private ABI.
4. Establish both eyes' crop, foveation and reprojection mapping at mask sampling.

The native received-key chain narrows the next investigation; it does not close
these links. Missing, reused, ambiguous or mismatched associations must render
opaque. No latest-mask or approximate-timestamp fallback is acceptable.

Validation is limited to targeted instruction-dataflow and vtable-relocation
checks against the identified binary, plus documentation checks. It is not an
installed bridge or a live video/mask correlation test.
