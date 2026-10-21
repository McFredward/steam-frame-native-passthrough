# Compatibility and evidence

## Supported combination

| Component | Current paired route |
|---|---|
| Headset and transport | Steam Frame over Steam Link |
| Frame SteamVR | **2.18.2 · build 1791331621**, exact checked runtime files |
| PC sender | Windows x86_64, supported NVIDIA encoder, HEVC |
| Game source | Explicit integration; Gamma SDR UNorm, MultiPass, real RGB contribution and alpha |
| Installation | Matching PC and Frame companions, [guided setup](PC-FRAME-SETUP.md) |
| Hardware status | User-confirmed native room pictures; continuity and simultaneous local menu/game are not fully certified |

The supported game and minimum version have their own [table](../README.md#supported-games).
A matching development build is needed before an upcoming game release is available.
Steam Frame standalone passthrough is separate and requires neither streamed companion.

A version label alone is insufficient: exact checked binaries, interfaces and
rendering contracts must match for native activation. The Frame version does not imply a PC SteamVR
version. Native Linux PC sender installation is not yet supported; running the
Windows game through Proton still uses Linux SteamVR as the driver host. Other
headsets, Virtual Desktop, ALVR, HDR and unknown stereo/source formats are outside
this route's current validated combination.

## Picture and transparency

The game supplies ordinary full-frame RGB contribution E and coverage A. Matching
companions associate coverage with the exact native encoded access unit and
selected decoded image. The Frame blends E+(1−A)×camera locally; camera video is
not sent to the PC. This is independent transparency, not colour replacement.
The route does not pack colour into a smaller atlas or apply the retired route's
resolution/foveation/sharpening lease.

Both companions, supported game source and valid current-image proof are required.
A successful install, compatible peer, generated mask or healthy startup is not
proof that the headset is displaying the right image. Missing/invalid proof keeps
ordinary opaque video. No previous or latest mask is substituted. Other codecs
must continue ordinary Steam Link without gaining passthrough permission.

Partial opacity is represented. It does not let game shaders refract the real
room: the game's shaders never receive its camera image. Arbitrary glass,
background-sampling shaders and every material family are not certified.

## Current evidence

Software checks exercise production native dispatch, source/texture ownership,
authenticated control, accepted access units, receiver leases and guarded GPU
coverage. The independent-mask route has user-confirmed visible room pictures.
The paired 0.2.9 hardware run was reported as the best continuous picture yet,
with a brief activation interruption and a later native video reset. Lossless
compression was verified in use, reducing coverage payload from 108.6% of
original video in the 0.2.8 capture to 53.5% in that different run; this is not
a controlled benchmark.

The latest read-only audit of game Build 713 / patch 0.2.10 verifies 8,481
decoded/uploaded masks without decode, upload or guarded-submit refusals. There
is no native backtrack reset after active masks begin. The logs still identify
a game-owned main-menu head replacement that withdraws its source, and two
decoder emergency handoff timeouts (one before active masks). These are bounded
observations, not proof that every visible frame was correct or uninterrupted.
Candidate 0.2.11 repairs two source-proven avoidable waits: lost notification
after exact capture retirement, and waiting for an exact token already known
to be permanently rejected. Its focused software controls pass; the candidate
has not yet been observed on the headset and does not claim to explain both
previous aggregate timeout events. Game Build 714 separately preserves its
owned menu head across native menu-anchor replacement.
The latest automated and log results do not certify arbitrary game materials,
new runtimes or a final like-for-like sharpness/latency comparison. See the
[continuity evidence and limits](development/FRAME-CONTINUITY.md).
The maintainer's October21 Demeo1.4.11 test confirms the PC0.2.14 repair:
correct opacity/matching eyes and no recurring flashes or noticed glitches.
The log confirms29exact output completions during QoS with0unmatched outputs;
all14,629certified masks reach Frame metadata. Frame0.2.12 is unchanged.
Short authored game fades and native loading/source switches remain distinct.
Demeo mod1.4.12 separately changes identified game fades toward passthrough;
its new fades and the other two games still require their own headset tests.
Simultaneous local Steam menu/game mixed reality remains unconfirmed.

The [old receiver's visible gameplay and sharpness limitations](history/ATLAS-RECEIVER.md)
are historical results of a different, retired method. They must not be presented
as validation of this route or as an available runtime fallback.

## Updates and recovery

An untested runtime no longer removes Install/Update/Repair from the assistant.
After the ordinary Apply confirmation it shows the detected PC/Frame versions
and requires a separate, default-No **Install anyway** confirmation. Inspection
alone cannot approve a write. The approval belongs to this exact package and
runtime snapshot; changed inputs or a retry need a new decision.

Installation is separate from native activation. If the private native bindings
or shader input cannot be verified, the assistant can install the owned payload,
pairing and receipt in an explicitly inactive state. It does not guess hook
offsets, generate a shader from an unrecognized native binary or leave a previous
active layer behind. The status must say inactive rather than imply a working
passthrough session. A later compatible patch can activate the prepared files;
Uninstall and recovery remain available. This exception concerns runtime versions,
not damaged packages, unknown user files or unsupported PC platforms.

The installer checks exact runtime fingerprints before activation. A SteamVR or
Steam Link update can invalidate those checks. A mismatch must refuse native
passthrough and preserve ordinary video; it does not make an unreviewed runtime
compatible. Install a compatible companion release when available.

Files live in user-owned directories; no system partition or SteamVR binary is
replaced. Exact ownership receipts/checkpoints protect update, uninstall and Undo.
Unknown or changed files are preserved. Old receipt-owned installations remain
recognizable for safe removal/migration and recovery only. Do not reactivate an
archived Frame-only release as a workaround.

Fresh-start monitoring checks software startup, followed by the user's picture
confirmation. It cannot guarantee every future SteamOS update. Keep the Frame
awake until a coordinated install/Undo confirms the interface is running.
[Setup and recovery details](PC-FRAME-SETUP.md#update-uninstall-and-undo).
