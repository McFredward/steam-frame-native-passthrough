# Stream-bridge admission and encoder quality adaptation

## Hardware evidence

The paired October 18 run used GloomhavenVR Build 708. The PC provider loaded in
`vrserver` and the current Frame layer loaded in both receiver processes. This
rules out a missing provider as an explanation for this run. The game retained
`scene-contribution-atlas-v3` throughout; no ordinary-RGBA format grant appeared.
The legacy uniform-stream lease therefore remained active.

The PC log alternates between `waiting-supported-encoder` and
`waiting-paired-peer` until the shared 32-message budget is exhausted within
eight seconds. It also records 83 failed native NVENC reconfigure attempts in
this session. The existing log does not contain their individual image
contracts; it cannot prove every attempted change was bitrate-only. Complete
coded-video widths also cannot establish a percentage loss of per-eye detail.

## Source-proven defects

1. `NvencBoundary::reconfigure` previously revoked the entire source session
   before every original call, including ordinary bitrate/framerate changes
   and rejected requests. `SenderRuntime` then destroyed its paired listener
   and handshake. Ordinary bitstream unlock does **not** clear `encoded`.
2. The Frame receiver permanently blocked its current decoder pool after an
   initial connection refusal. If the first native video input arrived before
   the PC listener was ready, later availability could not recover that pool.
3. Repeated startup transitions consumed the provider's entire logging budget,
   hiding later successful states or different refusals. Receiver status
   callbacks previously emitted no useful connection status.

The earlier production fixtures explicitly expected every reconfigure to revoke
the source. They tested that conservative policy instead of composing continuous
quality adaptation with the actual control handshake. They therefore could pass
while normal Steam Link adaptation prevented admission on hardware.

## Correction

The sender retains a session only for a strictly matched, successfully initialized
image contract. It deep-copies the pinned SDK 11.1 configuration without retaining
the caller's pointer. Only bitrate/VBV values and frame rate are normalized for
the comparison. Codec, geometry, preset, asynchronous mode and all other fields
must remain unchanged. Reset requests, unknown contracts and unexpected native
errors still revoke. A same-contract `INVALID_PARAM` refusal preserves the old
working contract; native arguments and return values remain untouched.

The actual reviewed Steam Link caller initializes the inner rate-control
`version` with `NV_ENC_CONFIG_VER`. That field is compared unchanged; the outer
configuration version establishes the layout. Requiring the nominal inner
version would wrongly reject the real caller even though synthetic fixtures pass.

Already accepted asynchronous submissions retain their exact input/resource/
output/timestamp receipts across quality-only changes. They are not reassigned
to newer pictures. Native reentry cannot create new exact-frame claims while
reconfiguration is in progress; real source changes still retire old receipts.

The receiver reconnects only for transient transport unavailability, with bounded
250 ms to 2 s worker backoff and fresh successful native input. It allocates fresh
admission identities, clears old certificates and quarantines retired decoder
tokens until completion. Authentication, capacity and source-proof failures
remain blocked. This does not change the displayed video or start a device timer.

The source-format permission already survives legitimate encoder/pool changes
for its kernel-validated pipe client. It is separate from exact per-frame mask
permission. Restoring native stream quality after the first grant therefore
does not require changing the game or fabricating image evidence.

## Validation and limits

Production-core/control loopback tests interleave 73 successful and rejected
quality adaptations through Hello, Ack, Commit and the coded-AU probe. They also
cover asynchronous frame receipts, invalid configuration pointers, borrowed
output lifetimes and source invalidation. A separate production receiver test
starts before its real localhost listener, later completes the full handshake
and rejects old certificates and retired tokens. The unchanged previous sender
and receiver sources each fail the corresponding new regression.

The sender and receiver runtime fixtures are now registered in CMake/CI. Sender
fixtures require an explicitly prepared, hash-checked public SDK header; configure
never downloads it. Existing rendering, shader, installer and GPU evidence is
inherited only where its code is unchanged. No headset picture or current
Windows/NVIDIA runtime success is established by these software checks.

A further production control regression confirmed that gracefully closing an
established producer peer could retain its fixed port in TCP TIME_WAIT, blocking
the next admission. Revocation now aborts only this private auxiliary control
peer and discards its obsolete queued messages. It changes no Steam-owned video
socket or listener-sharing option. Six fully authenticated localhost sessions
reuse the same endpoint immediately, with fresh authority and active duplicate
listeners still refused; the previous production source fails this regression.

After the passing broad CI run, only the affected lifecycle gate is repeated for
this bounded transport repair. Lifecycle-only changes now have a dedicated fast
CI workflow; rendering changes continue to trigger the full native gate.

Public API background: [NVIDIA's reconfiguration guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html#reconfiguring-an-existing-encode-session).
The actual supported ABI remains pinned to SDK 11.1.

## Follow-up: a stale local image endpoint

The later paired run loaded the correct updated provider and Frame libraries.
Its sender reported thousands of same-contract quality adaptations with
`same=1, reset=0`, instead of continuously destroying admission. This is hardware
evidence that the earlier QoS repair is active; it is not evidence of successful
full-resolution output. The game still retained its legacy atlas lease.

The Frame receiver explicitly reported `waiting-local-image-route`. A read-only
snapshot found a private `receiver.sock` created by the preceding process run,
but no matching kernel endpoint or process listener. The new compositor tried
to bind that occupied pathname, refused it and retried the same unrecoverable
path. Its local channel had never reached `Connected`, so the receiver correctly
withheld its network handshake. The PC's `waiting-paired-peer` was a consequence
of that earlier local failure. Receiver stdout is in Steam's output log; reading
only the compositor journal had hidden this diagnostic context.

The compositor consumer now holds a private, marked, nonblocking advisory lock
for the entire channel lifetime. A pre-existing socket is reclaimed only when
its exact private ownership, parent and inode are stable, `/proc/net/unix`
contains no live endpoint, and a nonblocking connection probe specifically
returns `ECONNREFUSED`. A live legacy listener is preserved without probing it.
Foreign files, symlinks, altered lock identities and ambiguous outcomes remain
refused. The lock inode intentionally persists after stop, preventing two new
processes from electing independent owners. All checks run on the existing
non-render channel worker.

The production local-channel fixture now kills an actual owner without cleanup,
recovers its endpoint, verifies both real peers and transfers complete metadata.
It also exercises simultaneous process starters, a legacy orphan without a lock,
live listeners and foreign replacement paths. The original production source
fails exactly at orphan recovery; the corrected source passes 162 checks and
ASan/UBSan with leak detection.

Connection logs now distinguish listening, connecting, endpoint/peer refusal
and loss, with one notice per state. Separate bounded sender counters identify
native draw/constant/source/output gates using existing observations only.
Formatting remains on the cold provider worker, with spaced reports and finite
budgets. The four D3D callsite return addresses were checked against the pinned
sender binary and remain unchanged. No unproven GPU admission check is relaxed.
This repair has software and captured-cause evidence; a successful headset
full-resolution path still requires a subsequent hardware run.
