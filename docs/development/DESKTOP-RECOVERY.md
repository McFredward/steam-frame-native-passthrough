# Ordinary desktop video during active MR recovery

PC0.2.21 and Frame0.2.18 distinguish an actual desktop/compositor frame from
missing game data. The October22 headset logs show foreign native PID6288
while the declared game is PID9280. At20:50:24–29 the Frame records708 more
camera-only draws and353 more None certificates. This is consistent with
the invisible desktop while input continues to work; it is not an exact
per-draw screenshot or a new hardware verification of this candidate.

The sender retains source classification through the complete original
luma draws, native CopyResource destination, canonical held IUnknown,
accepted NVENC input submission and matching completion. Only a complete
foreign-process picture can be ordinary. Missing masks, mixed-process
layers, unknown copies, stale source generations or recycled unconsumed
inputs cannot exempt game pixels. Both v1 and v2 explicit-RGBA producers
use this same route; application names do not select policy.

Negotiated optional bit512 permits signed policy3 with None coverage.
Policy1 remains active MR recovery and policy2 remains source withdrawal.
Policy3 keeps the physical recovery camera warm; its ordinary-video
permission applies only to the exact authenticated decoded-image lease.
The local IPC descriptor does not serialize policy: Process derives this
property from the matching signed metadata, including frame, binding,
native timestamp and content identity. Future metadata cannot reclassify
an already selected game picture.

Renderer publishes these ordinary images without GPU mask storage. The
current imported image and descriptor generations are checked at recording
and again at submission. An immutable per-command GPU update selects the
original video through the existing mask-independent shader. Global
camera-only output remains the default for an unproved or expired image.
Core QueueSubmit, core Submit2 and KHR Submit2 use the same rule; ordinary
desktop output does not require a camera acknowledgment. Returning to the
game immediately requires its own exact coverage without toggling camera
settings. RGB reconstruction, UVs, distortion and foveation stay native.

Software fixtures cover signed policy tampering, negotiation with older
peers, source/input reuse, source renewal, authenticated decoded handoff,
future metadata, camera continuity and all three Vulkan submission APIs.
Headset confirmation of desktop visibility and game return is pending.
