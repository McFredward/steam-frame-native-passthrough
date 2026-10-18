# Bounded dashboard transition diagnostic (not installed)

This package prepares one **closed → local dashboard open → closed** observation.
It is a developer diagnostic, not a passthrough fix or an installation command.
No helper starts/stops Steam, edits a service, changes a setting, enables a device
feature, or applies an expiry/restart timer.

## What is attached

`FRAME_DASHBOARD_TRANSITION_PROBE` builds the separate, `EXCLUDE_FROM_ALL`
`VkLayer_dashboard_transition_probe` target. Ordinary release definitions and
the shipping presenter bank stay unchanged. All loader integration is inside
diagnostic-only preprocessor guards. The diagnostic keeps the normal app-owned
shipping pipeline handle and creates a separately owned instrumented companion
at the same successful cold native creation completion.

The companion uses an additional compatible Set1 storage buffer. It is bound
only around an eligible ordinary draw, then the normal pipeline is restored.
Previous native graphics bindings at Set1 or above, descriptor-buffer routes,
queries, secondary recordings and unknown recording state refuse instrumentation.
The original API call and arguments are forwarded. Native Set0/push constants,
camera state, attachment state and every original color expression remain intact.

There are no extra image reads. Six first-winner records contain values already
computed by the exact shipping fragment: the consumed native selector, R0/R1,
flags, current original/final RGBA and any guard values it already sampled.
Selection is inside normalized native q883 `[.45,.55]²` on a32pixel raster.
Invalid R1 has a distinct flag; missing/unread values are **incomplete**, never
proof that the atlas guard failed. The selector is not independent physical-eye
proof. Each stage's two slots are immutable after their first GPU claim.

The1600byte buffer is initialized disabled. Phase updates are pre-recorded
GPU commands ordered on the actually retrieved native queue; no CPU writes race
live shader reads. Explicit terminal phase3 publishes both the records to the
host and the disabled control value to future fragments. Collection requires
that update's actual fence. Reused native draw recordings read the current GPU
phase. Old spent sessions never rearm.

The cold runtime checks the actual enabled `fragmentStoresAndAtomics` request;
it does **not** enable the feature. It currently requires the single ordinary
queue profile observed in the successful2026-10-17 native SSBO run: one queue
create info, one queue, family0 in that run (the new run observes its own family).
Ordinary core QueueSubmit and its public timeline-semaphore pNext are supported.
Submit2/unknown queues/protected or different device profiles explicitly refuse
measurement and preserve the ordinary call. Runtime status records those facts.

## Preparation only

Build the isolated target with Linux Vulkan headers and an explicit compiler:

```sh
cmake -S . -B build/dashboard-probe-arm64 -DBUILD_TESTING=OFF \
  -DFRAME_DASHBOARD_TRANSITION_PROBE=ON \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
cmake --build build/dashboard-probe-arm64 --target VkLayer_dashboard_transition_probe -j2
python3 -B probe/dashboard_transition_probe_session.py prepare \
  --run /absolute/new/private/run \
  --library /absolute/build/libVkLayer_dashboard_transition_probe.so \
  --shipping /absolute/private/shipping-robust.spv
```

The source shipping artifact must be31384bytes with SHA256
`44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49`.
The helper derives and pins the diagnostic artifact, copies the AArch64 library,
and emits the existing loader namespace manifest, private config and ownership
receipt. It performs **no installation**. A host library is accepted only with
the explicit `--host-fixture` flag and remains labelled a host-only fixture.

An eventual reviewed attachment needs the exact environment recorded in the
receipt (`GHVR_PIXEL_SNAPSHOT_CONFIG=/absolute/run/session.conf`) and a fresh
source-pinned AArch64 provider/load preflight. The process must be the original
`vrcompositor`; an already spent launch claim cannot be reused on a restart.
There is no hidden age deadline or automatic rollback.

**Coexistence:** the persistent public receiver uses the same
`VK_LAYER_GHVR_pixel_snapshot` namespace and must not remain active as a second
receiver. Before a separately authorized future attachment, save and suspend
its exact activation, preserve the user's preferences, and use only the
diagnostic activation. Never replace a mapped library. At manually agreed
rollback, remove only the temporary activation, restore that exact saved public
activation and restart once. Preparation/status/collect do not perform these
steps; the existing version-pinned startup/rollback checks remain mandatory.

## A future single measurement

Do not prompt for a headset until package review and provider/startup preflight
are complete. No current permission to install or restart is implied here.

In the running approved diagnostic session, issue these deliberate phases:

1. `phase --value0`: streamed game visible, local dashboard closed.
2. Wait for status `queued_phase:0`; open the local Frame Steam menu and issue
   `phase --value1` while it is open.
3. Wait for `queued_phase:1`; close it and issue `phase --value2`.
4. Wait for `queued_phase:2`, then explicitly `phase --value3` to disable and
   collect. No automatic time or restart action occurs.

Use `python3 -B probe/dashboard_transition_probe_session.py ACTION --run PATH`
with `status`, `phase --value N`, or `collect`. The helper requires each earlier
phase to have reached the real queue. Its timestamps are manual context receipts;
correlate them with local Frame systemui open/close logs. It does not infer menu
visibility from desktop focus or instantiate another OpenVR client in compositor.

## Evidence and limits

Outputs are bounded below64KiB:1600bytes of sparse records,64cold route entries,
three unknown-route counters, at most24small lifecycle statuses, a guaranteed
terminal status, and four small manual context receipts. Artifacts/library are
inputs, not output captures. Pipeline/shader identities are hashes and scalar
handles, never redistributed shader bodies. Route counters mean **submitted
recorded-command uses/attempts**, not a draw census or a GPU completion claim.
Handle generations are not recycled; capacity exhaustion is an explicit refusal.

Compare accepted companion tags and VS/PS hashes across phases to identify a
changed native pipeline route. Compare central R1, guard values and decoded vs
fallback branch for a guard rejection. These observations can narrow a local
opaque backdrop, but cannot alone prove the final visible layer stack: a later
native overlay may still cover the sampled scene. Unsupported/unobserved paths
remain explicitly incomplete. There is no independent public-overlay transport
proof bundled here; a separate RGBA overlay needs exact known-pattern or handle
association, not a matching size alone.

## Focused checks

`dashboard_transition_probe_run.py` accepts `--suite all|core|gpu|lifecycle` and
explicit shipping/library/record paths. It checks the actual isolated resource
code, ordered original shader instructions, valid SPIR-V, bit-identical actual
shipping/diagnostic raster, six sparse slots, GPU phase changes across a reused
draw command, terminal visibility and stable disable, bounded status lifecycle,
and causal negative controls. Host tests do not establish a correct headset
picture, current native route eligibility, or an update-safe installation.
