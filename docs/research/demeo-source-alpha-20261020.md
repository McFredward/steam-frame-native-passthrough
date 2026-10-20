# Demeo source-alpha boundary, 2026-10-20

The maintainer reports that Demeo 1.4.8/1.4.9 has matching eyes, but solid
objects remain translucent in both eyes. GloomhavenVR renders both eyes
correctly with the same installed companions. This is not evidence that the
shared companion is universally broken, or that Unity's opaque materials all
have an opacity of 0.5.

In the supplied 1.4.9 PC log, actual game-Submit probes repeatedly read alpha
255 in three small regions of each eye. The corresponding Frame log adds
6,309 fully-zero coverage masks and one mixed mask. Readback and Submit report
no errors. Sparse regions are not whole-image coverage or a same-pixel/frame
comparison across the game, sender and headset boundaries.

"Fully transparent" in that counter describes decoded **coverage bytes**, not
measured final headset opacity. Native video RGB is retained independently of
coverage. For a premultiplied contribution, composition can be expressed as
`scene_contribution + (1 - coverage) * camera`; zero coverage therefore does
not erase an independently retained nonzero scene contribution. The actual
hardware blend and a numeric 50% opacity have not been measured by that log.
Do not use the counter alone to assert that the user must see only passthrough.

PC patch 0.2.12 adds a read-only measurement at the actual native shared-texture
input. The existing live source declaration, DirectMode handle, generation,
PID, eye and shape checks precede the probe. Three 32-square regions per eye
are copied at most once every two seconds. Raw alpha min/mean/max and zero/
partial counts are published with their own PID, present serial, extent,
storage/view formats and the current native shader alpha-related constants.
These diagnostic samples grant no coverage permission or video association.

This separates two currently unmeasured possibilities: alpha may already be
lost between the game's Submit and Steam Link's shared source, or the source
may retain alpha that the later coverage draw does not preserve. It also
records whether native tint alpha can affect the coverage draw. No source,
tint, RGB, coverage algorithm, shader, tracking origin or camera is rewritten.

Storage is prepared cold: four 32-by-96 staging images and two event queries.
The graphics callback performs one DONOTFLUSH completion poll and, only after
completion, at most one READ/DO_NOT_WAIT map per eye. There is no wait, Flush,
retry loop, file/socket operation, dynamic allocation or full-image/RGB export
in this diagnostic. Optional preparation failure does not disable working
coverage. The worker emits a bounded error or at most 14 sample reports per
source PID, with at most eight source transitions; snapshots never spin or lock.

Focused evidence includes actual D3D11 zero/fractional/opaque readbacks for
six RGBA/BGRA storage variants, both eyes, unchanged original source bytes,
pending-query and cadence controls, and coherent concurrent publication.
Those are software tests with owned textures, not SteamVR or headset proof.
The next hardware log must contain `native source alpha:` before a downstream
boundary can be localized more precisely. Table ghosting remains unresolved.

Frame patch 0.2.12 and installer 0.2.13 are unchanged. No Frame file or service
was changed during preparation; the developer's device access remains read-only.
