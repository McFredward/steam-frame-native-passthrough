# Read-only PC compositor-state observer

This development diagnostic distinguishes public **PC SteamVR** fade/grid state
from an observed menu or receiver-image change. It never changes the image,
creates an overlay, writes a setting, installs anything, restarts Steam/SteamVR,
or accesses the Frame. Public state alone does not prove what a headset displays
or which process originally requested a fade.

GloomhavenVR can run its pinned copy automatically in Debug during genuine native
Frame/Steam Link MR. The parent owns that opt-in and closes its stdin on exit,
Off or session loss. This standalone source is independent of the game adapter.

## Build without starting SteamVR

```bash
bash pc/compositor-state-probe-build.sh /path/to/reviewed/public/headers /private/output
```

The script runs only portable fake-API/core tests, writes three example JSONL
transcripts, and cross-compiles `compositor-state-probe.exe` for Win64. It requires
`g++` and `x86_64-w64-mingw32-g++`. It neither downloads nor redistributes Valve
binaries. The public `openvr_capi.h` must have SHA256
`3d32d8eb5bcbe2e250610123aee8142c68f0e7d4da823f87513b6a2a333078d7`.
The [official SDK header](https://github.com/ValveSoftware/openvr/tree/master/headers)
and its [license](https://github.com/ValveSoftware/openvr/blob/master/LICENSE)
apply independently. A changed header needs an ABI review before changing the
pin. Exact interfaces are `IVRSystem_026`, `IVRCompositor_029`, `IVROverlay_028`
and `IVRSettings_003`; there is no guessed function-table or version fallback.

The Win64 EXE is statically linked against its toolchain runtime, has no build
timestamp, and needs the user's **installed** public `openvr_api.dll`, which is
not included. Compiler versions are still relevant to reproducible hashes.
After reaping tests, remove owned host test binaries/caches while retaining the
source, logs, transcript, manifest and current Win64 deliverable.

## Optional explicit manual observation

With the existing PC SteamVR session and stream already running, PowerShell:

```powershell
.\compositor-state-probe.exe --observe --openvr 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openvr_api.dll' | Tee-Object -FilePath '.\compositor-state.jsonl'
```

Press Enter, Escape or Ctrl+C in that terminal to end only this Background client.
There is no timeout. The observer uses the public `VRApplication_Background`
contract: it does not start or keep SteamVR alive. Initialization error121 means
no existing server. No Windows `vrserver.exe` census is imposed: a Windows game
and helper under Proton can use native Linux SteamVR via the public SDK bridge.
Windows and Proton runtime execution remain separate validation boundaries.

For a managed child, use `--stdin-pipe --openvr <absolute-dll-path>`. This requires
a real stdin pipe before loading OpenVR. EOF, any input byte, a broken pipe or
Ctrl+C stops the observer. `PeekNamedPipe` checks availability before reading a
single stop byte, with no blocked reader or background reader thread. Both modes
use a100ms polling cadence, **not a lifetime timer**. No elapsed time triggers
cleanup, restart or automatic exit.

The DLL path must be canonical, absolute, local drive syntax, name
`openvr_api.dll`, and identify a bounded PE64 DLL without reparse-point path
components. Wine's absolute `Z:\...` paths are accepted. The file stays open
read-only while `LoadLibraryExW` uses only that directory and System32 for loader
search. Registry, PATH and the current directory are not searched for substitutes.

## JSONL contract

Stdout is ASCII JSONL, including under redirected Windows pipes; no free-form
runtime error/device strings or serial numbers are emitted. Schema:
`sfnp-pc-compositor-state-v1`. Each line including LF is at most1024 bytes; total
stdout is at most32768 bytes. An explicitly launched monitor emits:

1. `ready` with `read_only:true, non_atomic:true`, or one terminal `refused` with
   numeric `stage` and `code`. A refused client still releases every owned local
   resource, but never emits an unready `stopped` record.
2. At most32 **changed** `sample` records. Exact content is deduplicated; read
   timestamps alone do not trigger output.
3. One `budget` with `samples` and already-emitted `bytes` if either cap is reached.
   It then remains alive but **makes no further API calls** until explicit stop.
4. One `stopped` with `samples` after a previously ready client ends. At most35
   total lines are possible. Output failure also stops further reads without a
   repeated write loop; process exit reports failure.

`sample` fields are fixed:

| Field | Meaning |
|---|---|
| `sample`, `begin_ms`, `end_ms` | 1-based record number and `GetTickCount64` read-span endpoints; not a video/frame association |
| `fg_r/g/b/a`, `bg_r/g/b/a` | `GetCurrentFadeColor(false)` and `GetCurrentFadeColor(true)` |
| `grid_alpha`, `numeric_valid` | `GetCurrentGridAlpha`; false means some nominally available float was nonfinite |
| `dashboard_visible`, `can_render_scene` | Public dashboard/scene capability getters |
| `scene_focus_pid`, `last_frame_renderer_pid` | Public scene-focus and last-renderer process IDs |
| `do_not_fade_to_grid`, `do_not_fade_to_grid_error` | Read-only `steamvr/doNotFadeToGrid` bool and raw API status |
| `force_fade_tracking`, `force_fade_tracking_error` | Read-only `steamvr/forceFadeOnBadTracking` bool and status |
| `scene_dimming`, `scene_dimming_error` | Read-only `steamvr/envBrightnessSceneAppDimming` float and status |
| `standalone_system_layer`, `standalone_system_layer_error` | Read-only `dashboard/standaloneSystemLayer` bool and status |
| `hmd_connected`, `hmd_class` | Actual connected/class getters for device0 |
| `identity_stable`, `identity_valid` | Repeated public property reads agree and pass bounded identity validation |
| `frame_link_profile` | 0 unknown/other;1 canonical Steam Frame+vrlink;2 observed Deckard MP+cv profile |
| `tracking_error`, `model_error`, `registration_error` | Raw public string-property status, without the supplied strings |

An errored setting's false/zero is a placeholder, **not a usable value**. Error5
means unset/no default. When `numeric_valid` is false, nonfinite values have been
replaced by0 solely to keep valid JSON; consumers must mark those numeric readings
unavailable rather than treating that0 as a measurement. No alpha threshold,
clamp or picture manipulation is applied.

Identity reads use HMD0's public tracking-system1000, model1001 and registered-type
1036 properties, each printable ASCII and at most96 characters with checked API
length/NUL/status. The narrow tuple is read twice to catch ordinary transitions.
Profile1 requires exactly `Steam Frame` and `vrlink` tracking or registration;
profile2 exactly `Deckard MP`, `cv`, and absent/empty or `vrlink` registration.
Registration accepts the exact `vrlink` or `vrlink/` prefix, case-insensitively.
These match the existing game helper's cooperative profiles; they are not device
authentication or receiver-installation verification. Original strings are never
logged. Even repeated identity and context getters are **non-atomic**, not proof
of the physical view or that one coherent video frame carried these states.

Refusal stages are1 arguments,2 library,3 exports,4 Background init,5 System,
6 Compositor,7 Overlay,8 Settings,9 required getter pointers,10 stdin,11 exception.
Codes are the native numeric error where available. No process other than this
observer is stopped. Removing the EXE is the entire uninstall; there are no saved
Steam settings, services, registered applications, overlays or persistent changes.
