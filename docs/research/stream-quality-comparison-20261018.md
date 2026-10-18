# Direct streamed MR quality comparison, 2026-10-18

## Observed behavior

The maintainer compared the same GloomhavenVR session with native mixed reality
off and on. Off is sharp; on loses substantial detail. The supplied PC logs
identify ModBuild 703, source `d86736508`, Windows/D3D11, RTX4090 and PC
SteamVR/OpenXR 2.17.10. This PC version is distinct from the accepted Frame-side
runtime version. The receiver's v3 stereo image and corrected edges were
previously confirmed on the headset; preserved normal image quality was not.

The eye-resolution slider changes actual XR render targets in this run:
4040, 5050, 6060 and back. The matching existing Frame logs nevertheless retain a
3264x6400 decoded video allocation throughout MR. Without MR the ordinary
foveated stream uses 1152x4608. These are different layouts, not ordinary per-eye
widths that can be compared directly. Foveated reconstruction can concentrate
more samples in the central view despite a smaller total allocation.

No Frame file, camera preference, service or installation was changed to obtain
these logs. Exact hardware input copies, hashes and read-only retrieval receipts
remain in the integrator's private Build704 evidence directory.

## Why another width request is insufficient

The v3 atlas reserves 2048/3168 of each carrier axis for scene RGB. Opacity uses
another portion of the same video. Equal source allocations remove the former
half-resolution opacity filter, but do not preserve the normal stream's scene
sample budget. Uniform encoding also removes its dense central reconstruction.
An accepted public target-width setting is not proof that the native encoder
delivered that width: actual decoded geometry remained unchanged while the
source and requested dimensions increased.

A retained private PC driver contains a proportional encoder-format cap that
quantitatively fits the observed geometry. Its exact identity against the
current PC runtime is not established, so it is not evidence of a universal
decoder or hardware maximum. Private binaries and disassembly are not part of
this repository.

Consequently the unchanged uniform v3 carrier is not a full-quality solution.
Increasing only its requested width again would repeat a falsified approach.

## Activation and the local dashboard are separate boundaries

The game previously started atlas preparation while its asynchronous stream
settings lease was still pending. Build 704 requires the current live lease,
matching extent and unfinished helper at admission and render callbacks. Pending
work retains the ordinary scene. This closes the source-side race only. A
settings receipt is not native encoder readiness, so it does not establish that
the reported one-second blurred-edge transition and flash are fully fixed.

One dashboard episode includes tracking loss and cannot represent all menu
opens. A later clean local controller open/close has no decoder-format change.
The logs do not distinguish a different native draw route, current format-guard
rejection or a later opaque local background. Do not allowlist another vertex
shader or rewrite R1 from those logs alone. The requested result remains live
game, local menu and camera together.

## Next transport candidate and its limits

The explicit developer helper in
[`pc/projection-overlay-probe.md`](../../pc/projection-overlay-probe.md) submits
two static RGBA projection surfaces through public OpenVR. It does not patch
the PC sender, change stream settings, install a receiver or render the game a
second time. It tests whether a public route can preserve independently
delivered alpha without consuming the normal scene video's RGB area.

The reviewed public API and a successful local build do not prove Steam Link
transports those surfaces independently. An overlay could instead be flattened
into opaque video on the PC. A matching receiver observation must establish the
actual known pattern or proven source identity; a new RGBA allocation or visible
translucency alone is insufficient. The helper has 115 portable assertions and a
strict Win64 cross-build, but no Windows, Proton or headset execution evidence.

Even a positive transport result still needs current-frame eye/pose/frustum
registration, sharp edges, native motion, stereo fallback and real-scene proof.
A separate mask associated only by arrival time, latest image or an application
frame counter cannot establish those properties. The full-quality repair and
game-plus-dashboard composition remain open; no new headset test is presented
as a completed repair.

## First dashboard diagnostic activation: measured refusal, not a menu result

The explicitly authorized isolated diagnostic loaded on the Frame on2026-10-18.
SteamVR2.18.2 started successfully; the actual compositor mapped the diagnostic
library and not the public receiver. The diagnostic reported `QueueProfile`
before resources/recording (`ready:false`, zero submissions and instrumented
draws). No game stream or menu experiment was requested after that refusal. No
sparse records or image capture were collected. This does not establish a cause
for the visible dashboard defect or a working full-quality transport.

The status confirms one queue-create record and one queue, with the required
fragment atomic feature already enabled by the native application. It does not
identify the remaining rejected predicate: queue flags, queue `pNext`, or the
observed graphics capability. Its reported family0 was an unassigned default
on refusal, not proof that the actual family was0. The earlier successful
transaction's counts and family did not establish a null queue extension chain.
In fact that older implementation explicitly supported one typed original
`VkDeviceQueueGlobalPriorityCreateInfoKHR`; the isolated probe rejected any
queue extension. Reusing that existing constrained scheduling contract fixes a
source-proven incompatibility, but cannot retrospectively prove this launch's
exact rejection. Subsequent diagnostics must report the cold inputs and each
rejected predicate before another hardware round.

An initial standalone service-stop transaction was replaced by the Frame's
concurrent graphical-session restart. No activation files had changed then.
The controlled swap used an exclusively owned volatile user-unit mask during
stop/swap, removed in `finally` before the explicit normal start. This was a
transaction interlock, not an automatic timer. No system unit or public package
binary was modified. The rollback likewise restored the exact original public
unit drop-in and manifest link, restored only the owned camera-setting delta,
and restarted the public installation after the maintainer's explicit approval.
Hashes, active mappings, published public activation and service success were
verified; the maintainer also confirmed a normal visible/usable surface.

Private deployment receipts retain the exact rollback command and source.
Residual private staging is inactive and under1MiB; it is retained while some
same-user process maps cannot be inspected, rather than unlinking a possibly
mapped library. It is not a persistent diagnostic activation. The public
installation remains enabled and its library byte-identical to the baseline.
