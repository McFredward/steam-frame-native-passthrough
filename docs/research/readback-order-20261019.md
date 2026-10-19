# Exact-frame readback availability review

## Current paired evidence

The retained 10:10–10:14 run uses Game Build710 and the current Frame companion
(SHA25636eab22a336afcb09f9ac3978f0b62d399d2be40e3027da56f1cee8e295d7343).
The PC reports19,012 native input copies,1,706 captured coverage copies,
1,705 matched encoder inputs,17 mask-bearing access units and1,516 generic
readback refusals. The old counter does not retain HRESULT, so it cannot prove
that every failure was DXGI_ERROR_WAS_STILL_DRAWING. The original source-format permission is missing for17,247
other copies. Many of those belong to the ordinary3240×3240 dashboard source
before the4040×4040 game source becomes active. Do not treat every missing source
permission as an application negotiation failure or compare whole-run populations
without those different active intervals.

The independent explicit application-format admission therefore does work for
some game frames. It did not establish reliable mask availability. The native
copy hook queues the original colour copy first, then its independent R8 staging
copy. At the successful native bitstream Lock it attempts exactly one
Map(DO_NOT_WAIT). Failure drops that AU's optional mask; native Unlock retires
its staging reservation. There is no later retry for that same AU.

The old frame-atlas method required no GPU-to-CPU coverage readback: coverage was
part of the encoded picture. Its success does not validate this new asynchronous
handoff. Both current copies and their source metadata must retain exact frame,
source-PID/declaration epoch, encoder generation and accepted-input association.
A later or latest mask is never an acceptable repair.

## Production versus fixture

The retained D3D11 alpha-semantics fixture uses32×16 images and blocking Map(0).
It verifies native-style alpha composition and RGB preservation. It does **not**
verify the production full-size, single-attempt nonblocking readback deadline.

The bounded correction schedules the authenticated R8 copy before the unchanged
native colour handoff, in one protected immediate-context interval. Successful
Map remains the readiness proof. Command order is a useful availability change,
not a universal claim that NVENC completion fences every unrelated GPU resource.
New readiness/error categories must distinguish pending GPU work, failed Map,
source revocation, missing receipt, compression and certification failures.
There is no render-thread busy wait, blocking Map, guessed previous image,
resolution change or automatic device restart.

Microsoft documents [CopyResource as asynchronous](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
and [Flush as asynchronous](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush).
Flush alone is not completion evidence. A real-GPU fixture may wait for its own
native-handoff-model event; that test wait is not production behavior or an
actual NVIDIA-encoder capture.

## Receiver evidence limits

Eight read-only LocalPicture lease mappings prove decoded-picture delivery only.
Maskless CoverageEncoding::None discovery certificates take that same route.
Those mappings cannot prove that R8 coverage was decoded, uploaded or sampled.
The current journals cannot distinguish all those stages, and repeated early
pipeline refusals consume the shared cold-note budget before later successes.

A second source review found a legal early-record/reused-command limitation:
Renderer currently chooses a companion and immutable exact mask at vkCmdDraw
recording time. A command recorded before the mask arrives cannot acquire it
later merely by being resubmitted. The existing positive fixture supplies the
mask before recording. Whether the captured native compositor actually follows
this early-record pattern is unproven. Do not loosen current-image or GPU-lifetime
gates to speculate around it. Bounded cold stage counters distinguish transport,
upload, image selection, recording and actual guarded submission.

The upcoming software candidate needs a new headset result. Neither successful
setup nor a metadata count establishes visible passthrough or sharpness.

## Follow-up hardware: 10:56–10:59

The next paired run falsified reliable availability from ordering alone. With
the coverage-before-colour repair installed, the sender reported15 successful reads,
1,691 pending reads and zero read errors. The last pending result was
`DXGI_ERROR_WAS_STILL_DRAWING` (`0x887a000a`), approximately6.4ms after capture.
Elapsed time is not proof that this independent staging copy has been submitted
or completed. The Windows software-GPU fixture waits for a model native event;
NVIDIA's actual encoder completion does not establish that same ordering for
the separate R8 resource. The fixture's successful result must not be presented
as native NVENC synchronization evidence.

The receiver independently reported14 decoded masks,14 accepted uploads,
eight Ready slots and22 recording selections, but zero guarded-submit attempts.
It recorded9,830 command-buffer beginnings. Thus permanent absence of native
re-recording is not supported as the primary explanation for this run. These
counts establish mask production, transport and GPU upload for some exact
frames, not final sampling or a correct headset picture.

Review also found that the current renderer refuses every `VkSubmitInfo.pNext`,
including canonical `VkTimelineSemaphoreSubmitInfo` already accepted by the
earlier transaction code. This is a source-proven legal-submit defect; the old
hardware counters did not record extension types, so they cannot prove it was
the actual submit shape in this run. The repair must preserve original timeline
values, semaphores, stage masks, extension pointer and fence. Unknown or malformed
chains remain opaque. New bounded submit/collect reasons distinguish this case
from a revoked exact-picture lease or a changed native descriptor.

The sender correction explicitly submits only authenticated active coverage
work and records a per-stage completion event. Its exact event must be complete
before the existing nonblocking readback is attempted. It must not wait inside
the encoder lock, poll in a loop, substitute a previous frame, or flush ordinary
unmodified video. Explicit submission is asynchronous and is not a guarantee
that every mask completes before NVENC's first output callback.
