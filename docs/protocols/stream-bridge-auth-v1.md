# Paired stream bridge authentication v1

This is the portable authentication layer for [bridge v1](stream-bridge-v1.md),
not a running TCP provider, encoder hook, installer, mask channel or MR renderer.
It authenticates bounded control metadata and the initial video-carried HELLO.
It neither encrypts metadata nor establishes picture/surface association.

## Pairing and trust boundary

An explicit installer Apply operation uses verified SSH to provision one random
32-byte shared secret on the PC and Frame. No secret is carried in SEI, an
ordinary network packet, a process argument, a log, or an install manifest. This
does **not** assume Steam Link video encryption/authentication. The existing
Steam Link video is only the route on which the receiver must actually observe
the initial, independently HMAC-authenticated HELLO.

The external loader selects exactly one pairing with `pairings/active.json`:

```json
{"schema":"sfnp-active-pairing-v1","pair_id":"<32 lowercase hex digits>"}
```

Its credential `pairings/<pair_id>.json` has schema `sfnp-pairing-v1`, that same
16-byte `pair_id`, a 32-byte `key_hex`, local `role` (`producer` or `receiver`),
and `protocol_major: 1`. Older credentials can remain for explicit rollback;
the runtime must not scan them and try every key. The native loader must verify
the selected schema, version, role, owner, and private access (`0600` on the
Frame/POSIX or an appropriate private Windows ACL). A mismatched key requires
explicit repair, not silent replacement. The auth core accepts an already
validated binary `Pairing`; it performs no filesystem or SSH operations.

The producer creates a fresh nonzero CSPRNG 128-bit binding for each actual
native-stream/control session. The receiver is prepared with the caller's
current `VideoStreamContext` (native connection identity and generation). Only
`bootstrapFromVideo` may learn a binding, and only from a MAC-valid **producer
HELLO** observed inside a complete current coded AU of that exact context.
The `VideoSourceEvidence` is an explicit native-provider trust boundary: a
boolean supplied by an arbitrary socket callback is not evidence of a coded AU.
The core cannot independently discover a Steam Link connection, authenticate a
GPU surface, prove a function hook, or observe the caller's access unit.

`openControl` cannot bootstrap an unbound receiver. Once bound, both directions
require a valid MAC and matching pairing, direction, binding, and strictly
increasing sequence on every message. A valid initial MAC authenticates a
paired key holder; it does not identify a particular human or attest a binary.
Local key compromise permits impersonation and requires explicit re-pairing.

## Canonical envelope

Every integer is little-endian. The complete envelope is **176 bytes**: a
64-byte header, one exact existing 80-byte `SFB1` or `SFP1` payload, and a full
32-byte HMAC-SHA256. It fits the existing owned-SEI 256-byte payload bound.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `SFA1` |
| 4 | 2 | Auth major, exactly 1 |
| 6 | 2 | Auth minor, exactly 0 |
| 8 | 1 | Sending role/direction: 1 producer→receiver; 2 receiver→producer |
| 9 | 1 | Reserved, zero |
| 10 | 2 | Header length, exactly 64 |
| 12 | 2 | Payload length, exactly 80 |
| 14 | 2 | Total length, exactly 176 |
| 16 | 16 | Selected public pair ID |
| 32 | 16 | Fresh session binding |
| 48 | 8 | Nonzero strictly increasing sequence for this sending direction |
| 56 | 8 | Reserved, all zero |
| 64 | 80 | Exact bridge handshake or probe payload |
| 144 | 32 | HMAC over the preceding 144 bytes |

The inner handshake role must match the outer sending direction. A probe is
producer→receiver, an echo receiver→producer. Unknown payloads, versions,
directions, lengths, reserved bits, zero/reused bindings, and zero/replayed
sequences are refused. The receiver bootstrap additionally requires the inner
message to be `SFB1` Producer HELLO; an authenticated ACK, COMMIT or probe cannot
establish a session.

### HMAC and key separation

HMAC follows [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104) using the existing
`frame_receiver::Sha256` primitive. It uses the full 256-bit tag and compares all
32 bytes without a data-dependent early exit. For each direction:

```text
domain = ASCII("SFNP-STREAM-BRIDGE-AUTH-V1")  // 26 bytes, no terminating NUL
directionKey = HMAC-SHA256(pairingSecret,
                         domain || pairId16 || sessionBinding16 || roleByte)
tag = HMAC-SHA256(directionKey, envelope[0:144])
```

This is key separation for this fixed protocol, not a general purpose KDF API.
The canonical header, including the role, is also authenticated. The public
`hmacSha256` vector helper bounds key input to 1,024 bytes and data to 4,096;
production auth uses only a 32-byte key and these fixed inputs. Output is only
assigned after successful completion.

## Lifecycle and adapter contract

1. `prepareProducer(pairing, freshBinding)` validates the role, key and binding.
   `localSendAuthority` reports local signing capability for `Handshake.begin`
   and `markSent`; it is **not** a received/confirmed peer. This breaks the
   bootstrap cycle without pretending the producer has already heard a peer.
2. Sign the planned initial producer HELLO with `seal`; hand those exact 176
   bytes to the actual complete-AU SEI provider. Only after a successful actual
   handoff may the sender call the handshake's `markSent`. Signing alone is not
   a send receipt. An unused signed sequence may be skipped.
3. The receiver extracts the exact owned SEI once from its actual AU, calls
   `bootstrapFromVideo` once, and feeds the returned inner 80 bytes and
   `VerifiedChannel` into its deferred handshake begin/receive. It must not
   first bootstrap externally and then reprocess the same envelope through
   `openControl` (that is a replay), nor manufacture a synthetic 80-byte AU.
4. The receiver signs its HELLO/ACK/COMMIT; the external bounded TCP provider
   sends each envelope. The producer authenticates every response through
   `openControl` before exposing its inner bytes to `Handshake.receive`.
   Both peer nonces and negotiated capabilities are checked by bridge v1.
5. Mutual HELLO/ACK/COMMIT is still required. A MAC, `peerVerified()`, a local
   send authority, API success or a probe echo never enables image changes.
   The actual coded-AU probe roundtrip, exact selected video surface, eye,
   source extent, source identity and native mapping remain separate gates.
6. Actual native-stream or control-transport loss requires `reset()` on auth,
   handshake and probe state before any more use. There is no timer, feature
   expiry, autonomous restart or automatic re-pairing. Missing/incompatible
   peers or failed proof retain ordinary opaque Steam Link rendering.

An auth session has one serialized owner; callers must not race prepare,
seal, open or reset. The module performs no allocation, file I/O, socket wait,
logging or system RNG call. All invalid operations leave session authority,
sequence counters and caller outputs unchanged. Invalid traffic is not itself
a transport-loss event and does not mutate authority; the real transport owner
handles loss/reconnect. Sequence gaps are allowed; equal/lower sequence is
refused, and sequence wrap is never permitted. A new session needs a fresh
binding. Immediate prior binding reuse for the same pair is additionally
refused; this bounded memory is not a persistent replay database or a CSPRNG.

`reset` and destruction erase the owned key with volatile byte stores. Derived
keys, HMAC pads, digests and live SHA objects are also wiped after use. Copies
and moves of `AuthSession` are disabled. The caller must erase its own loaded
credential and any install/transport buffers. These stores do not promise
erasure of compiler registers, SHA primitive internal temporary schedules,
swapped memory, crash dumps, caller copies or a compromised process.

## Software verification and remaining proof

The focused fixture uses six full-tag
[RFC 4231](https://www.rfc-editor.org/rfc/rfc4231) HMAC-SHA256 vectors, including
long keys/messages, plus an independently generated literal complete SFA1
envelope. Negative controls cover every envelope byte, truncated/oversized
inputs, authenticated malformed payloads, wrong source/role/pair/binding,
sequence replay/wrap, reflected traffic, non-HELLO bootstrap and reset/reconnect.
The key-erasure check first verifies that its chosen key pattern really exists
in object storage, then verifies removal on reset. It does not claim to inspect
registers or stack internals.

These tests do not prove an authenticated live TCP provider, private credential
permissions, actual AU ownership, a native same-frame image match, GPU transfer,
Steam Link SEI survival, or headset quality. Those require adapter/runtime
evidence. No codec, pixel, atlas, renderer or mask behavior is changed here.
