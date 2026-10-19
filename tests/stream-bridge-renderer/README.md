# Actual renderer integration fixture

This focused fixture compiles the production `stream_bridge_renderer.cpp` and
`stream_bridge_coverage_gpu.cpp`. It exercises public Renderer entry points,
its real definition journal and real mask pool. The public Vulkan fake executes
the renderer's recorded immutable GPU guard writes in queue order and observes
the value read by the original native command. It does not copy the renderer's
admission decisions into a second implementation.

Coverage includes flags=0 replay, current and expired image leases, source EOF,
unknown/reset commands, simultaneous-use refusal, descriptor/view generations,
actual upload completion, failed original submits, retirement-fence capacity,
independent shared-UBO offsets, and both core and KHR submit dispatch. A batch of
129 original submit infos checks exact order/pNext forwarding and a single fence
on the final bounded chunk. Two semantic controls remove current-lease checking
or the leading zero command; both must fail a positive/negative assertion.

Unsupported submit extensions, oversized batches and contention with cold
pipeline creation turn only that batch opaque. Its scratch one-guards are
discarded and the original command buffers follow an immutable zero guard.
Their recording references keep sampled mask descriptors alive until the native
application completes and resets/retires those recordings. Follow-up checks
submit a fresh exact picture without recycling that retained storage, including
core/KHR Submit2 and an unsupported suffix after an otherwise supported prefix.
A causal control reinstates the old whole-pool quarantine and must fail the
fresh-picture assertion. Actual failed copy/tail-fence quarantine remains
independent. `--fallback-recovery-only` selects just these recovery cases.

`--reservation-recovery-only` holds a real production cold fence poll while
the native queue owner submits, both before reservation and after an original
successful guarded submit. Core Submit and both Submit2 dispatches are covered.
A refused reservation changes only that batch to an immutable zero guard;
recording references retain its sampled descriptors without permanently
quarantining storage. A successful reservation returns a private owner/epoch/
generation-bound ticket. The same externally synchronized queue owner completes
it exactly once without taking the cold mutex. Its real tail-fence submit
release-publishes `Submitted`; the cold worker acquire-reads only that state,
polls actual completion, resets the fence and release-publishes `Free`.
Stale, duplicate and foreign pool tokens cannot claim a reused ticket; original
submit errors, tail failures and post-submit mismatches still retain/quarantine
actual in-flight storage. Semantic controls reinstate pre-reservation poisoning
or post-submit mutex failure and must fail the focused recovery assertion.
`--thread-sanitize` optionally checks these deterministic threaded cases. A host
that cannot initialize TSan's shadow mapping is explicitly recorded as
unavailable, without claiming race-free execution.

`--continuous-only` follows 128 successive source frames through the production
pool's finite `MaximumSlots` storage. Its public Vulkan fake executes actual recorded staging
copies and checks each changed R8 marker in both native eye draws, including
legal flags=0 repeats. Every active exact frame must retain coverage; explicit
inactive `None` intervals are a separate control with precisely matching opaque
positions. A real cold poll of the previous tail fence stays blocked through
the next initialized frame's recording, copy, reservation, original submit and
tail submission, for all three dispatches. Reintroducing a cold mutex at acquire,
copy or reservation must fail this whole-frame assertion.

Hot acquisition owns a recording reference before reading any slot metadata.
The cold writer CAS-claims zero references before replacement and rechecks GPU
ownership; state/layout and GPU counts are atomic. Queue copies own
`Pending -> Submitting -> Copying`; fixed tickets own `Free -> Initializing`
before publishing their immutable reservation. A worker poll cannot take away
an otherwise ready exact frame. Separate pool tests overlap 4,088 cold uploads
with retained hot references and block an actual `Submitting` copy while cold
reap/duplicate upload and other masks proceed. First-use scheduled recording
may retain a pending image. Actual submit permission still requires a successful
copy submit on the same original queue, whose transfer-to-fragment barrier
orders its first layout transition and writes before the native draw. The
telemetry fixture verifies those first copied bytes without a CPU fence reap;
it still reports storage as `Copying`, never `Ready`, until actual completion.
Failed copies and a different native queue never gain sampling permission.
Source/decoder admission must
also prevent an active frame without its exact coverage from reaching this
stage. This fixture does not certify the hardware's message timing or picture.

`--timeline-only` reproduces the production renderer's former blanket refusal
of a legal `VkTimelineSemaphoreSubmitInfo`: a current selected recording never
reached a guarded submit. The renderer now shares the original transaction's
pure canonical-extension validator, without linking its retired runtime. The
extension must be the sole chain element, have value counts matching the
original semaphore counts and provide non-null arrays for nonzero counts.
Tests retain the original extension pointer, values, semaphore metadata, native
command order and fence byte for byte. The leading zero submit has no waits,
signals or extension; adding an immutable one command does not change the
native semaphore counts. Malformed, chained, unknown and protected extensions
stay opaque and cannot poison the next valid picture. A causal control restores
the blanket refusal and must fail. This is proof of a legal software call shape,
not proof that a hardware compositor uses that extension.

Separate atomic submit diagnostics report the first observed/refused numeric
extension types, unsupported submit reasons and pre-submit collection failures.
The latter distinguish missing/empty recordings, ineligible commands, stale
epochs, expired source leases and descriptor/image/memory/capacity mismatches.
An accepted timeline extension never replaces those exact source checks. The
focused cases also invalidate a genuinely selected recording before submission
and confirm opaque rendering with the appropriate first refusal. Core/KHR
Submit2 admission is unchanged, with added extension/flag observations only.

`--telemetry-only` executes the production diagnostics without changing image
admission. It distinguishes authenticated maskless discovery from decoded R8
coverage, accepted uploads from pending/copying/ready storage, unique delivered
pictures from repeated notifications, recording-time selection from original
successful guarded queue submission, and each bounded draw-refusal category.
Two causal controls mislabel `None` as a mask or pending storage as ready; both
must fail. Sixty-four pending startup pipelines produce one deduplicated refusal
message and cannot consume the later exact-shader/companion admission messages.
Hot paths only update fixed atomic counters. The existing cold worker emits
changed milestones or logarithmic progress, at most 48 two-line snapshots per renderer.
Queue acceptance still does not prove GPU completion or a visible headset image.

The focused test also records a legal native command buffer before any correct
picture/mask arrives, then supplies and completes that exact upload. Reusing the
earlier ordinary recording remains opaque; a genuine native re-record selects
the now-ready mask. This is an explicitly reproduced current limitation, not a
late-binding fix or evidence that the actual compositor uses this call order.

The documented native 29-binding scalar Set0 is replayed with the exact private
original shaders. The production successful-shader callback must prove the
original fragment's constant-zero image accesses and promote pending count1
definitions before the companion can register a route. A control removes
that authorization and must fail. Merely observing count1, or supplying a
mutated original fragment, cannot promote it. The older count3 integration cases
remain unchanged. The generated coverage module defaults to an independently
specialized count1 interface, also usable with a larger count3 layout.

The native creation callback preserves the original pipeline handle and picture.
It creates only a separate exact-coverage companion. Only a verified current
image lease selects that companion at an eligible draw; missing coverage,
unsupported recordings, lost observations and failed preparation keep ordinary
native RGB. There is no picture/mask-atlas decoder or legacy pipeline fallback.
Coverage and layout retirement destroy only owned objects, leaving the caller's
native pipeline to its original destroy. A control that changes the public
native handle must fail. Custom allocation callbacks remain unsupported and
keep their original object untouched.

`--artifact-fallback-only` checks unavailable coverage, unchanged native pipeline
and exact original-submit forwarding, plus a valid scalar-coverage control. The
separate `tests/stream-bridge-process/artifacts.py` fixture uses the actual cold
file reader and verifies that an obsolete atlas file cannot replace missing
coverage. The renderer fixture's supplied artifact observations do not prove
installed-file admission.

External process admission, authenticated picture callbacks and lease validity
are explicitly supplied fixture observations. A regular temporary fd tests the
renderer's kernel-object join, not an actual DMA_BUF export. This is not a
Steam/Frame, full decoder, physical-eye or GPU-picture test. Existing actual R8,
fragment and codec proofs remain separate.

Run from any checkout with explicit private shader inputs (never commit them):

```sh
python3 tests/stream-bridge-renderer/run.py \
  --native-root /path/to/native-checkout \
  --output /path/to/private/run-renderer \
  --vs /path/to/original-vertex.spv \
  --ps /path/to/original-fragment.spv \
  --coverage /path/to/local-coverage.spv \
  --sanitize --mutants
```

The runner takes immutable source snapshots before compiling and records their
hashes with the private input hashes. It reuses existing structural/fake-API
builders without executing their unrelated test mains. Results belong under the
main checkout's ignored debug directory. After all child processes are reaped,
run completion cleanup and remove only this run's regenerable `cache/`; retain
its source snapshots, logs and results.
