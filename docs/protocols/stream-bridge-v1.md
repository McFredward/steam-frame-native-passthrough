# Stream bridge v1: capability handshake and coded-AU probe

This portable C++17 component provides metadata framing and session validation.
It does not install hooks, authenticate a network peer, alter RGB, export masks,
prove a selected video frame, enable MR or own a socket. Missing/incompatible
components retain the ordinary Steam Link path. No feature expiry or automatic
restart is introduced.

## Separate permissions

1. **Recognition:** decoding a syntactically valid HELLO identifies a claimed
   bridge protocol. A caller may insert a small discovery HELLO into an actual
   coded HEVC AU before mutual confirmation, only after explicit opt-in and
   source ownership proof. Recognition is not peer authentication.
2. **Authenticated negotiation:** `Handshake::begin` and every state-changing
   receive/send receipt require a caller-provided `VerifiedChannel`. Its
   nonzero128-bit binding must identify the authenticated transport session;
   the boolean must never be hardcoded true merely because a packet arrived,
   an IP address matched, Steam is installed or an API call succeeded.
3. **Mutual confirmation:** both HELLOs, both ACKs and both COMMITs must be
   exchanged. An outbound action is not marked sent until the exact message
   has been handed to that authenticated route. Both build IDs are retained;
   compatible producer/receiver build IDs need not be equal.
4. **Coded-AU roundtrip:** a fresh full-width sequence and128-bit token are
   physically observed in an actual coded AU, then echoed through the verified
   return channel. This proves only the stated transport observation.
5. **Image permission:** exact source/session/eye/extent, actual selected
   native frame/surface, lifetime and matching native mapping still require
   separate stronger adapter evidence. Neither `confirmed()` nor
   `ProbeTracker::observed()` is permission to change a source, stream quality,
   image or alpha. No submit counter is relabeled an encoder frame ID.

Authentication establishment and a usable return channel are integration
prerequisites outside this library. In particular an unverified UDP echo cannot
activate this protocol. Public API availability and HELLO content alone do not
close those prerequisites.

## Handshake wire format

Exactly80 bytes. All integers are unsigned little-endian. No native structs,
padding or host-endian copies are sent. Unsupported versions, wrong lengths,
nonzero reserved fields and invalid enums are rejected transactionally.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
|0|4|ASCII`SFB1`|
|4|2|Protocol major1|
|6|2|Protocol minor0|
|8|1|Kind: HELLO1, ACK2, COMMIT3|
|9|1|Role: producer1, receiver2|
|10|2|Reserved, zero|
|12|4|Total message bytes80|
|16|8|Sender build ID, nonzero|
|24|8|Sender offered capability bits|
|32|16|Sender's fresh nonzero session nonce|
|48|16|Peer nonce; zero only for HELLO|
|64|8|Selected capabilities; zero only for HELLO|
|72|8|Sender required capabilities|

Known capabilities are negotiation1, Annex-B SEI2, exact source association4,
opaque fallback8, explicit coverage16. The mandatory baseline is1|2|8 =11.
Advertising source-association or coverage support is not proof of a particular
frame or content. Required bits must include the baseline, be known and be a
subset of offered bits. Unknown offered bits are harmless unless selected or
required; a local v1 configuration offers only implemented known bits.

The selected value is exactly the intersection of the two offered sets. It
must contain both required sets. Major/minor are currently an exact1.0 match;
there is no speculative negotiation with a future minor's unknown wire schema.

HELLO has no peer nonce or selected set. ACK/COMMIT repeat the sender's original
build ID/offered/required values and bind both exact nonces and the selected
intersection. The opposite role is mandatory; equal nonces are rejected.
Changed peer fields are not treated as an implicit reconnect.

## State/lifecycle API

`begin(LocalConfig, VerifiedChannel)` validates everything before changing
state. `next(Message&)` returns the next locally unsent HELLO, then ACK after
peer recognition, then COMMIT after both ACK directions. `markSent` accepts
only that exact planned message on the same verified channel; an action in a
poll outbox does not count as transmission. Reliable delivery/retransmission
belongs to the caller. Duplicate exact received messages and duplicate send
receipts are idempotent.

`confirmed()` requires both HELLO directions, both ACK directions and both
COMMIT directions. A peer COMMIT may arrive before the local COMMIT is handed
off; confirmation remains false until that send receipt. Ordinary invalid,
foreign, out-of-order or stale input changes no handshake state. Failed
encode/decode leaves caller outputs unchanged.

Actual transport loss calls `reset()` immediately and removes trust. Reconnect
requires fresh caller-generated cryptographic-quality128-bit nonces and an
authenticated channel binding. The class remembers the immediately previous
local and peer nonce to refuse the simplest reuse/stale-HELLO cases; that small
memory is not a replacement for caller freshness across process restarts.
The library has no timer and never attempts a Steam/SteamVR restart.

## Fresh probe format

Exactly80 bytes, separate ASCII magic`SFP1`. It intentionally has no mask,
selected-frame or presentation-permission field.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
|0|4|ASCII`SFP1`|
|4/6|2/2|Major1/minor0|
|8|1|PROBE1 or ECHO2|
|9|3|Reserved, zero|
|12|4|Total bytes80|
|16|16|Exact producer session nonce|
|32|16|Exact receiver session nonce|
|48|8|Fresh nonzero sequence, full64 bits|
|56|16|Fresh nonzero token|
|72|8|Reserved, zero|

Only a mutually confirmed producer can prepare a `ProbeTracker` request. Its
sequence must increase and its token must differ from the previous probe.
The caller must actually insert those bytes into the proved coded AU; a prepared
request is not a sent-frame receipt. A receiver calls `makeProbeEcho` only after
observing those exact owned-SEI bytes in that AU. The function validates session,
role and channel, not the caller's AU observation. An ECHO copies both nonces,
sequence and token exactly. The tracker accepts only that pending request's
authenticated echo. Replayed, different-session, sequence/token-mismatched and
wrong-role echoes do not satisfy it. Reset the tracker with transport/session
loss; it must never be used without the live confirmed handshake gate.

## HEVC Annex-B wrapper

`inspectOwned`, `appendOwned` and `removeOwned` consume a complete caller-owned
Annex-B HEVC AU. This is bounded NAL/SEI container validation, not a full H.265
picture/slice decoder or proof the caller supplied exactly one coded picture.
The adapter must obtain AU boundaries from its actual encoder/decoder handoff.

The owned payload is `user_data_unregistered` SEI payload type5 with UUID bytes
`53 46 4e 50 2d 42 52 49 44 47 45 2d 56 31 00 01`.
Appending creates one dedicated prefix-SEI NAL (type39) before the first VCL
NAL, inheriting that VCL's layer ID and temporal ID. It adds byte-aligned
RBSP trailing bits and correct emulation-prevention bytes. Original AUD,
parameter sets, foreign SEI and VCL bytes retain their original start codes,
order and exact bytes. No original byte is rewritten to carry the message.

Limits are16MiB per input/output AU,4096 NALs,64KiB per decoded SEI container,
256 bytes per owned payload and exactly one owned message per AU. Both prefix39
and suffix40 SEIs can be inspected. Type/size extension bytes and RBSP escape
sequences are validated. Invalid NAL headers, truncated sizes, malformed escape
sequences and duplicate owned messages refuse the operation. Foreign UUIDs do
not become bridge messages. An owned payload mixed with other payloads in the
same NAL is refused rather than rewriting someone else's SEI data.

Every operation constructs its result separately; refusal leaves output and
original AU unchanged. Removal deletes only a dedicated owned NAL, retaining
original nonowned NAL/VCL bytes and inter-NAL/trailing zero bytes. Appending then
removing restores the original AU exactly. Allocation failure can throw before
the output swap; callers must preserve the original AU on exceptions as well.

The wrapper does not prove SEI survival through another encoder/transcoder,
video integrity, pixel registration or cryptographic origin. That must be
measured at the real handoff and selected native decoder surface.

## Focused verification

The two C++ executables use literal hand-checked wire/NAL goldens, not only
roundtrips. They cover bilateral confirmation, unsent commits, role/nonces,
required-capability differences, malformed/truncated headers, invalid-input
transactionality, reconnects, auth/channel refusal, probe replay and mismatches;
SEI escape/size extensions, foreign/mixed/duplicate payloads, parameter/VCL
preservation, exact removal, aliased vectors and bounded invalid input.
These are software controls, not an authenticated live transport or a headset
picture. Actual runtime integration must retain that distinction.
