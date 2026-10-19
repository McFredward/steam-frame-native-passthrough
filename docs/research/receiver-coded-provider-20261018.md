# Concrete Frame coded-input provider (2026-10-18)

This is a prepared, **uninstalled** Linux/AArch64 provider for the actual public
V4L2 decode boundary. It changes no video bytes, decoder parameters or renderer.
No Frame was contacted, no service/GOT/preload was installed, and no headset
outcome is claimed. Its input snapshots are not mask or picture permission.

## Retained source and exact public-call contract

The private retained `vrlink` is 2,425,024 bytes, SHA-256
`31addef0c9553d3770b5570487699e96d70271d4073cd6c03b080fdee6ae2153`.
The executable read/execute `PT_LOAD` is file/virtual offset zero, length
`0x247541`, SHA-256
`55646531fe10d78787726ba6dd68e061bc162831a083c3430c0320f7b33c62d9`.
These are this retained file's facts, not a fresh identity read from the Frame.
Private instruction windows and scalar receipts remain in ignored GH debug
storage; no Valve binary or instruction body is redistributed here.

`preparePinnedProcess` independently hashes `/proc/self/exe` and the actual
loaded main executable segment obtained from `dl_iterate_phdr`. It exposes no
entry points for an unrelated executable or different code. The cold prepare
function does filesystem work; decoder callbacks do not. Original libc call
targets must be captured before a separately reviewed loader publishes any
entry. Those original targets and the provider must remain valid for the
process lifetime. Cold prepare/publication/destruction are serialized by the
loader; a stopped provider keeps its original forwarding targets alive.

The admitted return addresses are relative to that proven ELF base:

| Return address | Public operation | Observed meaning |
|---|---|---|
| `0x32edc` | `VIDIOC_S_FMT` | HEVC, OUTPUT_MPLANE (10); successful result must report one plane |
| `0x32f18` | `VIDIOC_REQBUFS` | OUTPUT_MPLANE, MMAP (1), successful allocated count |
| `0x32fec` | `VIDIOC_QUERYBUF` | One plane; its returned length and `mem_offset` define the mapping |
| `0x33008` | `mmap` | Null requested address, read/write, shared, decoder fd and exact queried plane length/offset |
| `0x31e0c` | `VIDIOC_QBUF` | One acquired mapped OUTPUT buffer; supplied size becomes plane bytes-used; separate received key becomes `timestamp.tv_sec` |
| `0x32bc0` | `VIDIOC_DQBUF` | OUTPUT ownership returns to the application |
| `0x31368`, `0x312e8` | `VIDIOC_STREAMON/OFF` | OUTPUT lifecycle; attempted OFF revokes evidence conservatively |
| `0x315dc` | `VIDIOC_REQBUFS(0)` | OUTPUT pool retirement |

All structures/request values come from public Linux headers, with LP64 size
and request assertions. Multi-plane array pointers are copied through bounded
`process_vm_readv` of this process, not dereferenced as guessed private C++
objects. Unknown memory, short reads and unsupported shapes yield no snapshot.
The tracked mapping is established only by the observed queried offset/length
and a successful matching `mmap`. Ambiguous, overlapping, wrapped, unmapped or
still kernel-owned mappings cannot become input bytes.

The [V4L2 mapping contract](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/mmap.html),
[QUERYBUF](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/vidioc-querybuf.html),
and [QBUF/DQBUF](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/vidioc-qbuf.html)
define the public lifecycle. `data_offset` is included in `bytesused`; the copy
uses exactly their validated difference. The original V4L2 request and all
caller memory are left untouched.

## Forwarding without a guessed variadic ABI

There is deliberately no exported `ioctl` override or automatic constructor.
The AArch64 entry is a separately named, opt-in tail dispatcher. A scoped loader
can only receive it after successful exact-process preparation. Unknown callers
and request/return-site pairs go directly to the **captured original target**.
The entry restores the original stack pointer, all integer argument registers
(including hidden-result `x8`), vector arguments and overflow argument area
before branching. It also saves remaining general/vector registers and
NZCV/FPCR/FPSR. The branch uses AAPCS64's scratch `x16`/IP0. The C classifier
preserves entry `errno`; no unknown variadic argument is read with `va_arg` or
reissued through a guessed C signature.

Only the exact admitted public V4L2 requests use the documented three-argument
`ioctl(fd, request, void*)` path. Each original is invoked once with the original
arguments. Observational reads preserve the original return and `errno`, on
success and failure. No lock is held over an original ioctl; original blocking
DQBUF/reentrant lifecycle cannot deadlock on this provider's own state lock.
Fixed-signature `mmap`, `munmap` and `close` retain their ordinary signatures.

## Bounded ownership and loss

The registry has two decoder fds and at most sixteen buffers per fd. The initial
two owned snapshot slots were enlarged by the
[continuous-coverage repair](continuous-coverage-20261019.md) to one complete
native burst: sixteen slots of at most 2 MiB, allocated during preparation. This is a
bounded prototype limit, not a claim that all valid HEVC pictures fit 2 MiB.
Larger/unsupported input stays completely ordinary and unobserved. With no free
snapshot slot, copying is skipped; no old slot is evicted. Successful QBUF still
marks kernel ownership even if its snapshot was dropped.

Bytes are copied **before** QBUF while the proven buffer is application-owned;
the copy is published only after that exact original QBUF succeeds in the same
pool generation. It retains the separate native timestamp token, buffer index,
pool and content generation. It does not turn the buffer index or a local
counter into a network frame ID. Polling copies into caller-owned storage on a
worker; no borrowed mmap pointer survives the native handoff.

Before codec/pool changes, STREAMOFF, any overlapping/partial unmap (from any
caller), close or stop, tracked ownership and queued snapshots are revoked.
Late success from a revoked generation cannot republish a copy. DQBUF is needed
before reading a previously queued slot again. Failures to observe exact
lifecycle are evidence loss, never permission to reuse the latest bytes.
There are no timers, restart actions, sockets, callbacks to the UI or logging
loops. A future loader must cover fd/mapping lifetime events process-wide;
dup/dup2/fixed-remap aliases or missed lifecycle events are not supported facts.

## Recovered actual receive chain and remaining framing boundary

This is stronger than merely noticing that Linux permits chunks:

- Current packet dispatcher `0x53f18` reads the received 16-bit token at packet
  offset 2; it remains distinct from the decoder slot.
- The current completion callback `0x541e0` checks a positive record payload
  length (offset 16) and received block count (offset 26) against the required
  rounded-up 1,024-byte block count before calling `0x53958` at `0x54250`.
  Its pointer-holder is record offset 48. The current record vtable relocation
  at `0x24a268` selects that completion callback.
- The submit manager retains/moves that payload holder and length, orders by
  the full 16-bit received token, then submits through decoder virtual slot 24
  at `0x53b04`. The retained decoder vtable at `0x248c68` places `0x31cc0`
  in that slot. That function passes the supplied used size and received token
  separately to OUTPUT QBUF as audited above.

Thus the traced normal path submits a **completed/reassembled native network
payload** rather than blindly handing individual network fragments to QBUF.
This does not yet establish, for every native branch, that one such payload is
exactly one PC encoder's Annex-B HEVC access unit with no aggregation or codec
variation. Nor does it close the PC image→encoder→network token association.
The [stateful decoder specification](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)
allows differing OUTPUT/CAPTURE cardinalities, so the public ioctl alone cannot
supply those additional facts. That is a remaining actual-caller contract to
prove, not a declaration that the Steam Link stream necessarily fragments AUs.

Consequently every current snapshot has `completeAccessUnit=false`. It must not
be passed to `bootstrapFromVideo` with a fabricated completeness assertion.
A bounded next step can observe a fully contained owned authenticated SEI NAL
in these exact source bytes and prove that metadata's current-stream origin
without claiming an AU/frame identity. Fragmented NALs need bounded reassembly
and actual source-session rules; picture use still requires complete exact
source/decoder/selected/import/eye/UV evidence. No such new gate is implemented
or relaxed here.

## Focused software evidence

The fixture executes public-call originals, mapped ownership, exact forwarding,
failure/errno preservation, pre-QBUF copy, DQBUF reuse, bounded-slot saturation,
codec/reset, partial unmap, close, stale in-flight success and unsupported
memory/shape paths. It also checks that the actual test executable cannot pass
the `vrlink` identity gate. A separate AArch64 execution under qemu exercises the
real assembly entry with register and stack integer/vector arguments, hidden
result pointer and `errno`; a fake classifier deliberately clobbers its vector
argument registers. This is emulated ABI evidence, not a Frame test.

Original decoder bytes remain unchanged. No authentication, AU completeness,
same-frame mapping, live hook compatibility or headset performance is established
by these software checks.
