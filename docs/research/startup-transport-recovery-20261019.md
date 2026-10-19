# Paired transport startup recovery (2026-10-19)

## Observed hardware sequence

The paired Build 710 logs show a successful authenticated peer and coded-AU
roundtrip at 09:12:08. The auxiliary connection was lost at 09:12:28, before the
first 4040 x 4040 game swapsets were recorded at 09:12:30. Ordinary Steam Link
video continued. The game stayed `source=False` / `operational=False`; there is
no evidence that this run entered the independent coverage source path.

This establishes a failure before source activation, rather than a current
legacy-atlas installation or an old game build. The old receiver's generic
`source-proof-lost` message does not establish which individual decoder branch
closed that particular connection. The fixes below are source-proven defects;
that distinction must remain explicit in hardware reporting.

## Sender defects and repair

An auxiliary EOF permanently latched `SenderRuntime::Failed` until the native
encoder generation changed. Normal bitrate/framerate adaptation deliberately
preserves that generation. Therefore a transient startup source/socket reset
could keep passthrough unavailable for the rest of an otherwise healthy stream.

Only an actual control transport loss now permits a paced fresh attempt
(250 ms, doubling to at most 2 s). Each attempt creates a new random binding and
must complete the existing authenticated video/reverse-channel handshake and
coded-AU proof. There is no timer that activates transparency. Authentication,
protocol, unsupported-source and explicit-stop failures receive no retry grant.
Native RGB, resolution, codec/QoS values, reprojection and coverage association
rules are unchanged.

Discovery admission also raced native `LockBitstream` output ownership. A
borrowed output is now classified atomically as Busy under the existing boundary
mutex. The new listener remains stable until the original native Unlock; neither
the caller's bytes nor its lease are replaced. Revocation and successful fresh
admission clear only owned queued-probe state. An undelivered probe from the old
session can no longer poison the next session on the same encoder.

## Receiver defects and repair

The receiver reserved one of 32 certified-picture slots before checking an
input's certificate. Ordinary/uncertified decoder inputs do not necessarily
produce any CAPTURE output, so those slots could fill permanently even while
ordinary video continued. Only cryptographically certified picture receipts now
occupy that table. Unknown inputs retain a fixed 16-bit token quarantine for
their exact physical decoder pool; a first completion cannot prove there are no
additional outputs. Real pool retirement is tracked per file descriptor, never
inferred globally from another decoder's newer generation.

The decoder snapshot queue is intentionally bounded. When an original native
QBUF succeeds but a byte snapshot was unavailable, the hook now publishes a
bounded receipt for that exact fd, pool and token together with its loss counter.
No gap or loss is reported before the original call succeeds. A known gap can
revoke the current authority and permit a completely fresh admission; it cannot
certify an image. The worker samples counters before draining these ordered
receipts, so a concurrent publication cannot fabricate an unknown gap. Missing
or malformed receipts, overflow and unattributed loss remain hard refusal.

Image-route pauses drain and quarantine skipped inputs instead of hiding their
losses with a replacement counter baseline. Reconnects quarantine pending old
receipts, retain native output ownership, and require new source nonces and the
complete existing video/control proof. Capture-pool reset, late old-pool events,
multiple physical decoder pools and repeated gaps are covered explicitly. Cold
reports now include the distinct refusal reason and bounded queue counters;
the earlier generic source-loss message was insufficient to identify the branch.

## Focused verification

The actual provider/control fixture uses real local TCP and the production NVENC
boundary, with an injected monotonic clock only for deterministic retry timing.
It checks EOF before game startup, same-encoder recovery, full fresh handshake,
old and second-session binding rejection, repeated loss before probe delivery,
borrowed bytes across reconnect, native Unlock, invalid MAC, explicit stop,
source/codec changes, ordinary QoS and unchanged native AU bytes.

Three causal controls run the same fixture with the old runtime, Busy treated as
permanent refusal, or old queued-probe state retained. All three fail at the
corresponding recovery boundary. This is software evidence, not a headset image
or end-to-end sharpness claim. Unchanged rendering/context ABI checks retain
previous evidence; a new complete game gate is neither required nor claimed.
