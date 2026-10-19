# Retired Frame-only atlas receiver

Historical record, retired by the maintainer on 2026-10-19. This is not a current
installation or game-integration guide. Use the [paired setup](../../README.md#installation).
Old release assets may remain available as archives; they are not a fallback or
an installation recommendation for the current development version.

## What worked

The original receiver needed a game integration and a Frame patch, without a
PC Steam Link sender companion. The game packed RGB contribution E and coverage
A into the same current per-eye video image; native Frame composition produced
E + (1 − A) × camera. It did not remove a key colour, and the camera stayed on the Frame.
The maintainer confirmed real GloomhavenVR gameplay with native camera blending
on 2026-10-17 (game Build686), as well as earlier synthetic fractional transparency.
Later corrected stereo/edges also had visible headset confirmation. Those results
belong to that route and do not prove the new paired route's final picture.

The v3 layout used equal 2048×2048 logical colour and opacity grids in a 3168×3168
carrier. Each eye therefore spent video space on opacity as well as colour.
The side-by-side comparison still showed a loss of sharpness relative to normal
streaming, increasingly visible on distant small objects. Raising the game's eye
allocation could not recreate colour samples already discarded by packing.

## Revisions and problems

- v1 was the earlier accepted format. Experimental v2 produced different pictures
  between the eyes and was quarantined; a successful local format test did not
  establish safe native stereo reconstruction.
- v3 corrected that format problem and later edge sampling removed the visible
  fringe. The improved edges did not eliminate the shared-video sharpness cost.
- The old edge-quality lease temporarily requested uniform encoder width and
  disabled sender foveation/sharpening. It restored only values it still owned,
  preserving changes made by another application. It was specific to the packed
  source, not a universal Steam Link quality requirement.
- Keeping a packed source behind a local Steam menu caused incorrect/background
  reconstruction. A later focus guard suspended it when XR focus was lost or
  unknown. This was containment, not simultaneous native game/menu composition.
- A valid format strip identified a layout, not independently the sender session,
  exact encoded frame or currently sampled image. It was not an authenticated
  substitute for the new access-unit and selected-image proof chain.

## Retirement and migration

The maintainer chose to remove the old source, decoder, quality control and
runtime fallback entirely, rather than retain two active methods. New builds
ship only the paired ordinary-RGBA source/receiver. Missing authority leaves
ordinary opaque video, never an atlas, chroma-key fallback or a latest-mask guess.

Installers still need to inspect and safely remove prior receipt-owned files,
suspend old activation during migration, restore original camera/settings and
complete a previously authorized recovery. This maintenance support does not
permit generating a new atlas shader, installing the old receiver or enabling it
as an alternative. An interrupted migration must preserve recovery records and
restore the original running Frame interface when Undo is selected.

## Evidence

- [Visible real-game result](../research/real-game-visible-run-20261017.md)
- [Persistent user setup](../research/persistent-user-setup-20261017.md)
- [Stereo divergence review](../research/eye-divergence-20261018.md)
- [Quality comparison](../research/stream-quality-comparison-20261018.md)
- [Current compatibility and evidence limits](../COMPATIBILITY.md)

The new paired route has software and partial hardware evidence for stable D3D
observation, authenticated control and mask certification. These milestones do
not yet certify full-resolution headset quality or the simultaneous menu picture.
