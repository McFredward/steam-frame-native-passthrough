# Coverage pool flicker after the first independent-mask picture

## Paired evidence

The 11:32–11:35 run uses GloomhavenVR Build 710 and the public receiver with
SHA256 `c17c7b1fa13b5855c97dd486458c701b7969c84848f434f4760b86a97d0b061c`.
The user confirms that the room is now visible, but repeatedly alternates with
opaque black. This is a first visible result for the independent-mask route,
not proof of stable presentation, sharpness or simultaneous dashboard support.

The PC's explicit submission/completion correction worked in this run:
2,077 completed query/readbacks, 2,077 certified masks, zero pending queries,
zero query errors, zero busy Maps and zero Map errors. There is no evidence for
another GPU wait or readback-worker redesign. The 163 other matched outputs did
not reach Map; source/stage/session checks and reconnect intervals are separate
populations. Ordinary dashboard frames intentionally do not grant scene coverage.

The Frame now reports real `VkTimelineSemaphoreSubmitInfo` (`1000207003`), with
zero unsupported-extension refusals. The previous legal-submit fix therefore
also has hardware evidence. At 11:33:30 it had accepted 478 uploads and 488
guarded command submissions, with five of eight mask slots quarantined. At
11:34:08 it had accepted 1,855 uploads and 1,013 guarded submissions, with six
slots quarantined. Draw counts include both eyes and repeated display of a
decoded picture; they must not be equated to unique video frames.

The whole-run ReadyMask miss counter includes 9,234 misses before the first
active scene masks. Its later increase of approximately 5,584 is relevant;
quoting all 14,818 misses as active-MR failures would be incorrect. Brief native
transport reconnections also cause legitimate opaque intervals, but do not alone
explain continual flicker. The game reports source authority becoming available
once; it does not log repeated per-frame user-setting changes.

## Source-proven pool defect

`prepareSubmit` originally permanently quarantined slots even when it merely
failed to acquire the cold worker's mutex or ran out of submission tickets.
This happened before any one-guard command reached Vulkan. Renderer correctly
replaced the attempted one-guards with zero, but still called `submitted`, which
could quarantine them again for the missing reservation. Recording references
already retained the immutable descriptors and storage. Such a refused
preparation should affect only that opaque batch, without destroying future
capacity.

A second, independent contention window existed after a successful reservation
and native submission. `submitted` tried to take the pool mutex again. Losing
that race prevented it from attaching the completion fence and quarantined the
slots. Lifetime protection is necessary after actual GPU publication, but making
ordinary worker contention permanently consume capacity is not a usable steady
state. The reserved fixed ticket must be handed to the original queue owner so
that it can publish the tail fence without reacquiring the cold mutex. The cold
worker may poll only fully published tickets. Real native-submit, tail-submit or
fence failures still require conservative quarantine.

No fix may retain a previous/latest mask for a new picture, derive coverage from
RGB, change resolution or accept an unproved source/frame/eye association.
Software contention/recovery fixtures and actual headset confirmation remain
distinct evidence.
