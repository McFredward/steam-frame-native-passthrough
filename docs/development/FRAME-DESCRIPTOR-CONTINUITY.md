# Stream coverage and native descriptor lifetime

## Paired hardware evidence, October 19, 2026

Frame patch 0.2.3 and GloomhavenVR Build 710 show active-game flicker even while
valid coverage continues arriving. Between 16:00:38 and 16:00:43, 450 native
recordings include 374 guarded submissions and 76 opaque ones. Descriptor
refusals increase by 152: two eyes for each opaque frame. None masks, mask
rejection and refused uploads do not increase. A second five-second window
contains 73 opaque frames and 146 descriptor refusals with the same zero-loss
coverage counters. Private paired evidence is retained under
`steamlink-sender734/paired-flicker-analysis.json`.

The source contained several overbroad invalidation paths. Resetting or
destroying any descriptor pool erased every tracked descriptor; any descriptor
copy or write batch larger than 256 did so as well. An unrelated native UI
resource operation could therefore remove the valid video association. The
old logs identify the refusal boundary but do not identify the particular
Vulkan API that triggered each hardware refusal.

## Scoped ownership

Track successful native descriptor allocations by set, pool and monotonically
increasing allocation generation in a bounded journal. Record that allocation
generation in command-buffer uses. A successful pool reset or destruction
retires only its own sets; a successful free retires only the listed sets.
Reusing a numerical handle must not revive a previously recorded command.

Writes and copies inspect their actual destination binding. Unrelated bindings
do not affect the video's binding 9. Unsupported changes retire the affected
destination rather than every native set. Larger valid batches are processed
without an artificial global-invalidation threshold. Unknown templates remain
conservative for their exact destination. Bounded diagnostic counters distinguish
pool, free, copy, template and allocation-capacity events.

The causal fixture keeps real valid frame/mask ownership while resetting an
unrelated native UI pool, copying descriptors and sending large write batches.
The previous global invalidation loses its active video guard; scoped ownership
preserves it. Separate controls still reject a retired video pool, allocation
ABA and stale recordings. Original native Vulkan calls retain their arguments
and return values. This is software evidence, not a new headset result.

## Separate states

The subsequent absence of application coverage follows PC dashboard opening at
16:00:57.840. That dashboard is a different source without the game's opt-in;
reusing the previous game mask would be invalid. The descriptor fix does not
claim simultaneous native dashboard/game MR or permit stale coverage.

The later actual video reset at 16:01:38 recovers a fresh authenticated paired
session and coded-AU proof by 16:01:40.759. This confirms the 0.2.3 allocation-
retention repair is effective in this capture. The descriptor correction does
not remove that reset protection or alter video extent, reprojection, foveation,
source pixels or mask association rules.
