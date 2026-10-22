# Tested versions and evidence

## Officially supported, successfully tested runtimes

| Component | Current paired route |
|---|---|
| Headset and transport | Steam Frame over Steam Link |
| PC SteamVR | **2.18.4 · build 1792185270**, user-confirmed Battles picture and scenario transition with PC 0.2.17 |
| Frame SteamVR | **2.18.2 · build 1791331621**, user-confirmed paired picture |
| PC sender | Windows x86_64, supported NVIDIA encoder, HEVC |
| Game source | Explicit integration; Gamma SDR UNorm, MultiPass, real RGB contribution and alpha |
| Installation | Matching PC and Frame companions, [guided setup](PC-FRAME-SETUP.md) |
| Hardware status | User-confirmed Battles continuity in the October 22 follow-up; simultaneous local menu/game remains unconfirmed |

The supported game and minimum version have their own [table](../README.md#supported-games).
A matching development build is needed before an upcoming game release is available.
Steam Frame standalone passthrough is separate and requires neither streamed companion.

These are recorded successful tests, not a runtime version allowlist. The PC
and Frame independently recognize the interfaces and rendering properties they
need after an update. Whole-file hashes record provenance and preserve the known
path; a changed hash alone does not disable the patch. PC SteamVR 2.18.4 and
Frame SteamVR 2.18.2 are officially supported following the
[October 22 successful Battles follow-up](development/BATTLES-BURST-BUDGET.md#confirmed-follow-up-october-22).
Earlier PC 2.17.10 tests confirmed Demeo, Battles and Gloomhaven pictures; the
new PC version has been retested with Battles, not all games. The Frame version
does not imply a PC SteamVR version. Native Linux PC sender installation is not yet supported; running the
Windows game through Proton still uses Linux SteamVR as the driver host. Other
headsets, Virtual Desktop, ALVR, HDR and unknown stereo/source formats are outside
this route's current validated combination.

## Bitrate and overlay compatibility

The v2 rate policy supports Auto and fixed Steam Link bitrate settings. The
confirmed October 22 headset run used Auto; constant-target CBR software tests
cover fixed bitrate with changing mask load. Fixed-bitrate headset validation
remains separate. See [bitrate behavior](../README.md#steam-link-bitrate-and-ovr-advanced-settings).

**OVR Advanced Settings is a known incompatibility in Demeo:** unwanted tracking
origin changes/recenters occurred with it running and were no longer observed
with it closed. Close it while using the Demeo native passthrough mods. This
records a conflict observed in these tests, not a claim about every overlay.

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

Accepting **Install anyway** enables the installation for testing; setup does
not actively leave it disabled because a runtime version or file digest is new.
The installed PC sender discovers public imports and the versioned DirectMode
RTTI table, verifies its current mapped executable and retains exact allocation,
source/eye and accepted-output proof. The Frame discovers its ELF imports and
uses public V4L2 queue structures, authenticated local peers and the versioned
OpenVR settings API. Equivalent shader compiler/debug/ID changes normalize to
the independently reviewed executable logic; arithmetic or interface changes do
not inherit that permission.

Compatible updates can therefore continue automatically without a new hash
allowlist. A genuinely unrecognized necessary API, shader contract or current
frame association still withholds that specific native operation and reports its
reason. The patch never substitutes an old mask or guesses an unknown ABI.
[Update recognition and its tested limits](development/NATIVE-UPDATE-RECOGNITION.md).

Files live in user-owned directories; no system partition or SteamVR binary is
replaced. Exact ownership receipts/checkpoints protect update, uninstall and Undo.
Unknown or changed files are preserved. Old receipt-owned installations remain
recognizable for safe removal/migration and recovery only. Do not reactivate an
archived Frame-only release as a workaround.

Fresh-start monitoring checks software startup, followed by the user's picture
confirmation. It cannot guarantee every future SteamOS update. Keep the Frame
awake until a coordinated install/Undo confirms the interface is running.
[Setup and recovery details](PC-FRAME-SETUP.md#update-uninstall-and-undo).
