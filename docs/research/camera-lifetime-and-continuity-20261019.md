# Camera lifetime and remaining descriptor gaps (2026-10-19)

## Hardware evidence

The maintainer's 0.2.4 run reproduced a black, then green, local camera after
the install restart. Standby/resume repaired the local camera, but the game's
camera region remained black until the entire Steam Link stream was restarted;
restarting the game alone did not repair it. This is not a successful hardware
fix. No device files or services were changed during the following captures.

The fresh restart has the intended order (Steam stopped, fresh SteamVR started,
then Steam started), without another Steam crash/autorestart. Native camera
processing nevertheless reported IPE/ICP errors, skipped camera updates and a
roughly 25-second LateInit interval. Three pre-stream bridge startup notices do
not establish that its startup preparation or permanent camera setting was
harmless. Removing the permanent camera override is a causal candidate, not a
proven explanation of every native camera failure.

Linux actually suspended and resumed at 16:44:39. The camera/XR service resumed
while the compositor PID survived. Existing stream and settings state therefore
cannot, by themselves, prove that the camera is ready after resume.

A later all-MR receiver sample contains 14 complete measurement intervals with
no None certificates: 6,202 masks decoded, no upload rejects, 6,232 guarded
submissions and 69 opaque submissions. The descriptor refusal counter increased
by 138 (two eyes per affected frame); image, exact-frame, mask and busy refusal
deltas were zero. This localizes those gaps to descriptor availability. It does
not identify which descriptor subreason caused them.

Private hardware inputs and machine-readable reviews remain in the main game's
ignored debug directory (`steamlink-live-20261019-163832`, `164905`, `165118` and
`steamlink-sender735`). They contain no public native binary payload.

## 0.2.5 changes

Camera permission is now session-owned. An independent process calls the pinned
native settings API only after authenticated MR demand. Its acknowledgment gates
both decoder publication and actual masked rendering. None coverage, source
loss and disconnect withdraw permission. An actual suspend offset forces an
acknowledged restore/enable sequence; a scheduling stall does not.

A final stereo review found an additional activation edge: a boolean readiness
change between the two eye draws could admit only the second eye. A unique grant
captured at command begin now binds both draws and submission. A later grant,
even after a successful disable/enable, cannot revive the old recording. This
is a bounded follow-up to the already passing camera and installer checks;
its focused stereo and acknowledgment controls do not rerun unrelated suites.

The original preference is journaled before mutation. Startup recovery is
offline and fail-open so a missing or damaged optional helper cannot block the
Frame interface. Update/removal use the live API before deleting the helper,
retain exact Undo authority and preserve unrelated preferences. The installer
migrates its own older permanent override once, rather than repeatedly changing
a user's later choice. There is no automatic headset restart or expiry timer.

Descriptor tracking is reviewed separately from camera state. Native UI writes
must not occupy the finite video-image journal with untracked image views.
Resource journals remain bounded and never evict an active proof to admit a
different image. Capacity and descriptor subreason counters distinguish a
reproduced production-path pressure defect from a future hardware attribution.

## Validation boundary

Tests use the actual isolated subprocess/IPC client, production process worker,
authenticated access units, exact leases and renderer/queue forwarding. The
native settings dispatch is an explicit test port, not a running Frame camera.
Causal controls must fail when the API acknowledgment, stale-response rejection,
resume restore barrier, native persistence proof or descriptor isolation is
removed. Preserve source/result pins and clean the completed generated caches.

Hardware acceptance still requires a clean install restart, both-eye gameplay
without intermittent opaque frames, standby/resume without reconnecting the
stream, menu transitions and intact resolution. A software pass alone cannot
establish these pictures. An actual unscoped proof loss still fails opaque;
never hide it by reusing a previous frame's mask or guessing another eye.

## Battlemarked pool replacement (2026-10-22, Frame 0.2.15)

Read-only captures for the 12:20–12:21 test show helper IPC closure at
12:21:16, followed by XRService pausing the physical passthrough cameras and
resuming them with a newly initialized helper at 12:21:17. No decoder reset
is logged at that point. The compositor reports retired shared pool epochs.

The camera's acknowledged native settings belong to the authenticated stream,
not to one decoded buffer pool. Retiring a pool now drops its obsolete lease
mapping without closing the helper, disabling the camera, or clearing the
last decoded coverage demand. This never admits that pool's images: all exact
lease, source/frame/eye and GPU publication gates still reject retired storage.
A new decoded None withdraws demand normally, as do source/session changes,
channel loss and consumer shutdown. There is no timer or stale-mask fallback.

Actual process tests assert uninterrupted helper commands across pool renewal,
old-picture rejection, fresh pool admission and immediate channel-loss revocation.
They use signed metadata and real shared leases/IPC; hardware continuity and
GloomhavenVR pictures still require a headset test. The PC binary, selector,
shader and camera helper are unchanged in this Frame-only update.
