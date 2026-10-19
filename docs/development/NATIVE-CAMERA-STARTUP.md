# Native Frame camera failure after setup restart

## Observed on October 19, 2026

The maintainer reports black, then green native passthrough behind the Frame's
local Steam menu immediately after installation/restart, before starting a PC
game. Suspend/resume restores the normal camera picture while the same patch
remains installed. Read-only capture confirms the installed 0.2.3 Frame library;
no device files, settings or services were changed during this investigation.

Private evidence is retained under the GloomhavenVR checkout's ignored debug
directories `steamlink-live-20261019-151422` and `steamlink-sender733`.

## What the logs establish

- Setup-related Steam/SteamVR service restarts occur at approximately 15:08:28
  and 15:09:29. Their native camera initialization is followed by repeated
  image-processing firmware response failures and failed internal recovery.
- The captured system journal contains 308 failed IPE frame-processing
  responses, 153 failures after hardware recovery, and 77 skipped compositor
  camera-frame updates. Kernel camera-processor interrupts and IOMMU context
  faults at address zero occur alongside these errors. This establishes a
  native camera-processing failure, not a healthy camera hidden by a UI panel.
- The two error cohorts run approximately 15:08:38–15:09:00 and
  15:09:40–15:10:38. The native compositor reports late camera initialization
  about 24.7 and 60.5 seconds after its initial camera setup respectively.
- Suspend at 15:12:41 shuts down hardware processing; resume at 15:12:51 boots
  the cDSP, rebuilds passthrough interfaces and restarts camera polling. The
  maintainer confirms a normal camera image after this operation.
- The last logged IPE failure is **before** that final suspend. Therefore,
  absence of errors after resume does not prove that suspend first ended all
  failures. A stale or otherwise invalid displayed camera image can remain
  plausible, but its exact pixels/buffer ownership were not captured.

The journal's PID 2001583 belongs to the Python `steamvr-logs.service` relay.
Its `XRService` identifier names the relayed native log, not that Python
process's camera implementation or lifetime. Do not infer camera-process
injection or persistence from this PID.

## Patch boundaries and remaining uncertainty

Without a streamed exact LocalPicture, kernel lease and authenticated coverage,
the receiver cannot select its streamed-coverage replacement. No such stream
was active in this report. The installer nevertheless enables the persistent
native `camera/forceSceneAlphaBlending` preference independently of a stream.
The coordinated service restart and this preference remain possible
contributors; the capture does not isolate either as the original trigger.
The kernel/firmware failure does not establish a physical hardware defect.

The existing startup health check proves service/compositor startup and UI
progress. It does **not** prove healthy camera pixels. A later diagnostic should
report native camera faults separately instead of calling process readiness a
camera success or treating the compositor's late-init line as a pixel test.

Do not guess a fix by restarting system camera/DSP services, forcing suspend,
repeatedly restarting SteamVR or changing native camera formats. Any device
experiment requires separately coordinated permission. The user's confirmed
manual suspend/resume remains an observed workaround, not an automated repair
or an explanation of the original trigger.
