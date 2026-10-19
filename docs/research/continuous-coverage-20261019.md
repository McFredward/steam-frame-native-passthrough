# Continuous exact coverage, October 19

## Captured cause

The 12:02 read-only Frame capture confirms the installed public 0.2.0 layer.
The earlier pool repair is active: every one of 2,620 received masks uploaded,
with zero upload refusals or quarantined slots. Between 12:01:33 and 12:02:17,
however, explicit maskless certificates increased by 789 and missing-ready-mask
draws increased by 1,577, almost exactly two eyes per maskless picture. Pool
upload success alone was therefore the wrong acceptance criterion for continuity.

The final PC log has 2,634 completed readbacks and 2,634 certified masks, with no
pending GPU queries or busy Maps. It also has accepted native inputs without a
matching coverage stage. Final PC and Frame snapshots end at different times;
their totals must not be treated as a network loss ratio.

The PC's repeated `transport-lost` transitions are real control-session closures,
but do not prove Wi-Fi loss. The receiver itself closes that socket when its
two-slot input snapshot queue loses a known individual input. Fresh handshakes
then take roughly a second in this capture, consistent with the long black
intervals reported by the maintainer.

## Source defects and correction

* The PC's global coverage mutex covered a full 5 MiB CPU readback. Contending
  native Draw/Map and exact input callbacks discarded valid frame facts. A lost
  retirement lock could later erase every stage, including unrelated inputs.
  Generation-tagged per-stage claims now keep captures, exact NVENC bindings,
  reads and retirements independent. Cold resize must first own every stage;
  it cannot free an accepted or borrowed input. The receipt is published before
  the original colour-copy handoff can let another native thread encode it.
* Certificate construction shared a try-lock with cold socket/control work.
  An immutable producer signer is now published only after the authenticated
  native AU capacity is known. Certificates own that signer's lifetime and
  recheck its current identity before committing. Close revokes it before
  resetting control. This removes the long-held control mutex, without claiming
  that C++17 shared-pointer publication itself is guaranteed lock-free.
* The receiver drained decoded CAPTURE events before already-queued OUTPUT
  snapshots. A decoded picture could disappear before its certificate was
  admitted. Bounded early observations retain the complete physical lease and
  are revalidated against the native provider before publication. Requeue,
  reset and loss retire the corresponding facts; token/timestamp guesses and
  latest-frame substitution remain forbidden.
* Input snapshots now reserve one complete native buffer burst (16 slots,
  at most 32 MiB allocated cold). Bounded input draining interleaves capture and
  control progress. An exhausted work budget prompts another drain before the
  worker sleeps; an idle worker still sleeps. Slots are never evicted.
* A contiguous, fully attributed individual loss quarantines only its exact
  decoder/pool/token, not the entire confirmed connection or all image epochs.
  This is containment for exceptional loss, not the normal availability path.
  Unattributed counter changes, missing/malformed loss receipts, capture-event
  loss, real source changes and actual transport failures retain their original
  emergency refusal and fresh-admission requirements.
* The running compositor's mask acquire, upload submission and submission-ticket
  reservation must not fail merely because the cold fence worker holds its
  mutex. Bounded reference/writer claims protect immutable mask metadata and
  GPU storage; queue completion retains its exact generation-bound ticket.
  GPU fences, source/image/eye association and initial-layout checks remain
  mandatory.

## Verification contract

Test continuous changing exact-frame masks, not just successful individual
uploads. The native PC fixture drives the production NVENC Encode/Lock/Unlock
boundary and ledger while another accepted read is paused. The control fixture
certifies 64 exact frames while an actual production socket poll holds its cold
mutex; the old source fails that same test. Receiver fixtures exercise early
CAPTURE, exact known gaps and complete native snapshot bursts, with stale lease,
wrong source and unattributed-loss negative controls. Compositor fixtures hold
the real cold fence callback while complete upload/draw/native-submit/tail
sequences continue on core, core2 and KHR2 submit paths.

These are software and captured-cause proofs, not a new successful headset run.
In particular, first use still requires real Vulkan layout/fence readiness;
unknown proof loss must never expose a stale mask. The hardware acceptance target
is a continuous active-MR game sequence with no routine opaque flashes and no
unnecessary connection renegotiation, retaining ordinary stream sharpness and
correct stereo.
