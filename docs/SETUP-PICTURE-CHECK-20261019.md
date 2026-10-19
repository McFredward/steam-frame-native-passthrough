# Installer picture confirmation

The startup software probe cannot establish that the headset displays a usable
picture. The Textual installer therefore presents three distinct answers after
the prepared startup: **Yes**, **No**, and **Cancel setup**. This is separate
from the ordinary Apply confirmation.

- **Yes** permits the successful operation to finish and release its recovery
  checkpoints.
- **No** performs another bounded, read-only startup/health observation. Even a
  passing software result does not override the user's visible failure. The
  next menu offers **Retry verification**, **Restore previous setup**, and
  **Exit**. Retry performs a fresh observation and asks about the picture again;
  it does not reinstall files, replay the operation, or restart services.
- **Cancel setup**, Escape, or Ctrl+C stops without automatic rollback or
  restart. Existing changes and recovery records are retained for deliberate
  later recovery. A cancelled picture prompt is never interpreted as No.

A failed recheck or unavailable SSH connection reports the failure and retains
the same explicit choices. It does not automatically restart the Frame simply
because the user answered No. The existing supervised recovery for an initial
software startup failure remains independent of this picture check.

Choosing **Restore previous setup** authorizes the existing exact checkpoint
restoration and coordinated restart. Completion now requires both the original
software startup check and an explicit **Yes** to the original picture. If the
original picture is still unusable, No offers another read-only verification;
Exit or cancellation keeps the recovery records. Restored files alone are not
reported as complete recovery.

The non-Textual fallback also preserves Yes/No/cancel as three distinct results,
including real arrow-key terminal navigation. Both interfaces are localized in
English and German.

## Focused verification

The affected Workflow and real Textual Pilot suites, entry-point integration,
and terminal/PTY suite pass. An additional actual PC/Frame installation fixture
checks that No leaves installed files and exact recovery records intact until
Undo is explicitly selected, then verifies the restored configuration before
the user confirms the original image and the records are released.

Three causal controls must fail that same actual-file fixture: removing the
fresh read-only recheck, omitting original-image confirmation, and treating No
as cancellation. These are software and UI-path checks; they do not establish a
visible headset result or exercise a live device restart.
