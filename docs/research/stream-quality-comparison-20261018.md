# Direct streamed MR quality comparison, 2026-10-18

The later [completed local-dashboard observation](dashboard-observation-20261018.md)
records the first full closed/open/closed capture and exact public restoration.
Its mixed-route first-winner limit remains explicit; it is not a menu/quality fix.

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

## Exact current spatial budget, not a subjective quality percentage

The actual v3 profile's logical canvas is3168-square and its scene contribution
is2048-square. These logical coordinates scale with the real carrier;2048 is
not a claim that the actual encoded image has that fixed extent.

| Allocation | Logical samples | Share of carrier area |
| --- | ---: | ---: |
| Useful scene RGB | 2048×2048 =4,194,304 | 41.79% |
| Useful R8 coverage | Four2048×512 strips =4,194,304 | 41.79% |
| Halo/guard/remaining layout area | 1,647,616 | 16.42% |

The four physical mask rectangles include their filtering halos and occupy
4×2080×544 samples, or45.10% in total. The16.42% figure above already includes
these halos: it must not be added a second time. The twelve128-square format
cells occupy1.96%; the rest includes edge-continuation/layout space.

The scene's linear fraction is2048/3168 =64/99 =64.65% on each axis.
At the same uniform carrier extent this is35.35% fewer samples per axis and
58.21% fewer useful RGB pixels than a full-area color image. It is **not** a
measurement of58.21% worse perceived quality, an additional fixed2048 resolution
limit, or an exact MR-Off comparison. Native foveated Off and uniform On use
different spatial density; filtering, codec and reconstruction also matter.

This quantifies the motivation for preserving the ordinary RGB path and moving
coverage to an app-owned separate channel. Such a route still requires the
exact selected-image and shared-ray association described in the
[receiver feasibility review](app-only-receiver-feasibility-20261018.md) and
[public API review](app-only-public-api-feasibility-20261018.md). No such new
runtime adapter or quality result is established by this arithmetic.

## Larger same-stream carrier: mathematically valid, not admitted by the inspected host

The maintainer proposes enlarging only the transported carrier to retain the
selected scene raster and add coverage outside it. This would compensate the
current spatial budget if the *actual* encoded image grew:99/64 per axis,
2.392822265625 times its area. It is distinct from rendering more scene samples
and reducing them into an unchanged video. A4040-square scene would require an
ideal6249.375-square carrier, or6272 at upward64-pixel alignment.

The [new host sizing audit](larger-carrier-host-limits-20261018.md) traces the
entire retained settings/layout/fit chain. Each associated layout limits its
square input to3200; the larger complete dimensions in other layouts pay for
their own packing. Public target/min/max settings and queried GPU capabilities
do not raise those later internal limits. The [receiver review](larger-carrier-receiver-negotiation-20261018.md)
shows capture allocation following the decoder-reported stream format and finds
no established outgoing override of that independent host bound. Retained
source identities versus current runtimes, active chart versus decode padding,
and foveated versus uniform sampling remain explicitly separate.

Consequently another width multiplier, layout toggle or enlarged Frame buffer
is not a supported full-quality repair through this inspected unchanged host.
Raising its implementation limit would cross the user's no-PC-sender-change
boundary. No production/build/Frame behavior changes or new headset test follow
from this research. A compact in-band mask is a separate, unproved capacity
strategy; a separate channel still needs resource/latency and exact-association
proof rather than being selected automatically.

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

## Public-overlay candidate: delivery is still unproved

Build706 actually executes the public per-eye helper and logs both geometries
and its local ready token. The maintainer nevertheless sees only the camera,
with no pattern, and kills the game. The game diagnostic had automatically
selected zero RGB/coverage after local readiness. That was an invalid readiness
assumption: API success never established visible delivery to Steam Link.

The supplied asymmetric frusta also expose a separate geometry defect. The
UV-centred[0.4,0.6] charts do not centre the forward viewing direction. The
forward ray misses the visible pattern interiors in both eyes. No visible image
therefore does not distinguish an absent route, flattening, clipping or bad
placement. Earlier core/GPU tests never established that hardware prerequisite.
Their204 native and257 Unity checks are historical evidence for the specific
old math/packing, not evidence that its headset picture worked.

The current explicit
[delivery preview](../../pc/projection-overlay-probe.md) is deliberately smaller:
one ordinary square RGBA overlay on a common physical plane in front of the
head, with the same image in both eyes. It changes no game/XR projection,
resolution, Steam setting, sender binary or Frame installation. The game starts
it directly on the button, closes options automatically and always retains its
real source image. Reopening options or explicit Stop ends the owned child;
there is no automatic expiry or later blanking. A producer flush precedes the
single texture handoff, and bounded readbacks describe local setup only.

This is an overlay-delivery baseline, not an independent-alpha witness or a
production quality fix. A visible colored/partly transparent quad alone cannot
prove that Steam Link transports alpha independently of the scene video. That
must be established separately before building dynamic full-scene output on
this route. Do not restore automatic source blanking merely on an API receipt.
Current-frame eye/pose registration, sharp edges, native motion, delivered detail
and stereo fallback remain additional requirements for a production solution.

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

The subsequent diagnostic-only correction reuses the constrained existing queue
contract and retains precise cold failure/query evidence.22 actual `prepare()`
vectors pass293 checks each in normal and ASan/UBSan runs; three causal negative
controls detect the old blanket extension refusal, a removed graphics guard and
invented family0. Strict host/AArch64 builds pass. The ordinary shipping
preprocessor output remains byte-identical to the previous reviewed source, and
the diagnostic scene shader hashes are unchanged. All116 strong imported
name/version pairs match the previously verified actual Frame providers. These
are software/preparation proofs, not a new device admission or menu result.

## Second attachment: admitted queue, exhausted optional startup census

The corrected probe actually prepared its owned resources on the Frame. Its
cold receipt identifies one ordinary graphics queue, family0, native capability
flags15 and one unchained KHR global-priority node with value1024. The native
application already enabled the required fragment feature. The new queue guard
accepted these actual inputs without rewriting them; the old blanket extension
check would reject this particular recorded input.

Later startup reported refusal7 before any manual capture phase. That enum
conflated the8 companion-pipeline limit,128 shader-hash summaries and64 route
summaries. The ordinary presenter recorded6 successful replacements; this
strongly suggests optional summary exhaustion but does not identify which
summary filled, nor prove how many instrumented companions had been created.
Those separate counters were missing. The maintainer confirmed a normal
stream/passthrough picture, then approved manual removal. Original public unit,
manifest, library hashes and active compositor mapping were restored; the
maintainer confirmed normal UI. No six-slot sample was collected.

The correction keeps all allocation limits.128 shader summaries and56 generic
route summaries may truncate with explicit drop counters. The other8 fixed
route indices belong exclusively to the actual companions and retain validated
recipe VS/PS identities even when the generic cache is full. Indices and tags
are never evicted/relabelled/recycled. The actual ninth-companion, feature and
resource refusals remain strict. Lifecycle status separates occupied slots,
successful companion creation and both optional truncations, without emitting
one file per dropped event.

The same local GPU lifecycle replay now fails on the previous code and passes
on the correction:192 fixture-authored shader observations and96 route events
precede real owned buffer/queue/shader work, all6 sample slots and fenced final
collection (45 checks, zero Vulkan validation errors). These synthetic metadata
events are not a recording of all native startup calls; the GPU portion proves
the real phase/buffer path survives that explicit workload. A separate actual
`prepare/resources/pipeline` path with a successful fake driver passes290 checks
normally and under ASan/UBSan, including late accepted companions after512
shader/240 route events and strict rejection of a ninth companion. Four causal
mutants are detected. The unchanged470 raster checks are inherited; no new
headset-picture, full-draw-census or dashboard-repair claim follows.

The maintainer also explained that minimizing the fullscreen game to answer in
chat opens the Steam window. Existing logs independently distinguish remote
show requests from local controller toggle/hide events. A stream-ready reply
therefore cannot establish a closed menu. Future operator phases must check the
latest logged UI event and refuse a mismatched phase; `bOpenDashboard false`
alone is not a hide event. Keep chat accessible during the comparison and close
the menu before confirming a closed section. Log context still does not prove
final compositor pixels or stable animation completion.
