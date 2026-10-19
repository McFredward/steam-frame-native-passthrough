# Pinned swap-descriptor admission, 2026-10-19

The current paired hardware run proves the writable D3D context facade remains
installed: all four callbacks stay active, the table has149 public slots and the
runtime changes103 original entries. It also exposes a separate source-admission
bug. The game creates4040x4040 format27 textures with fourth descriptor word9;
our former validator mistakes this for nine samples and discards both game swap
sets. The source inventory consequently contains compositor3240x3240 format29
textures, not the game's own process. A healthy connection and22 compositor mask
certificates do not grant the game an ordinary-RGBA source lease.

## Version-pinned interpretation

Independent read-only inspection of the exact supported native sender confirms
that its fourth descriptor word packs two fields:

- `samples = max(1, word & 7)`
- `arraySize = max(1, word >> 3)`

Thus0,1,8 and9 all describe one sample and one slice. The native SRV builder also
selects `TEXTURE2D` for that1/1 case; the existing source-view gate is appropriate.
These are facts about the already hash-pinned driver, not a redefinition of the
generic public OpenVR descriptor. No private binary, instructions, disassembly or
shader is distributed. Private hash/range receipts remain with the hardware input.

## Repair and boundaries

One primitive-only decoder is shared by registry admission, cold texture import
and per-draw shared-resource matching. Native calls still receive the original
descriptor pointer and bytes; the registry retains the raw descriptor. Only our
expectations are normalized. Exact kernel handle, process, generation, width,
height, format, array size and mip checks remain in force. Coverage still requires
one sample and one slice. Unsupported arrays, malformed fields and unsupported
sample counts remain refused; no colour, UV, resolution or receiver change is made.

The production ownership fixture now uses the captured word9 and preserves its
original arguments, both-eye metadata and lifetime checks. The actual Windows
sharing fixture also uses the decoded word9 and an independently opened shared
allocation. Its focused workflow is `sender-resource-tests.yml`, `focus=shared`.
The previous raw-sample validator is restored as a single causal mutation: it
must fail the actual game-owner admission assertion, rather than merely produce
a different helper result. Cold-import and actual allocation matching have
separate normalization controls as well.

The native dispatch, receiver, protocol, encoder and game checks from the prior
delivery remain scoped evidence for unchanged areas. This bounded repair uses
the focused descriptor/sharing checks; it does not repeat those entire suites.
Successful software checks do not establish headset sharpness or combined menu
composition. No Frame files or services are changed by this investigation.
