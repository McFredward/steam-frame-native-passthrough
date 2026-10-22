# Branch defaults and native logging

PC 0.2.18, Frame 0.2.14 and installer 0.2.18 introduce Error, Warning,
Important and Debug thresholds. Game-mod logging is configured independently
in its greenscreen.ini. Both dev packages retain existing diagnostic output;
main packages default to Important. Existing Valve/Unity/loader logs are outside
our control.

Important retains bounded startup, attachment, source/connection state and
failure messages. Routine QoS/bitrate/alpha counters, renderer statistics and
local decoder flow/latency counters require Debug. The cold renderer worker
still emits compact warnings for new rejected masks, uploads, guarded submits,
GPU pool refusals or pipeline loss. Filtering never changes image/source/eye
proof, mask availability, video rate, shader logic or native call ordering.
Rendering/decoder callbacks only query an atomic threshold. Files/environment
are read once during cold initialization, with bounded input and a quiet
Important fallback on invalid data.

The package contains receipt-owned pc-provider/logging-default.conf and
frame/logging-default.conf. The latter is installed in the receiver root.
Native binaries use the same implementation for both channels. tools/package_stream_bridge.py
chooses dev/Debug or main/Important at packaging time; an explicit channel is
available to release tooling. ZIP users need no Git checkout at runtime.

When a maintainer later publishes/merges that artifact onto the other branch,
the Match package logging to branch workflow changes only the two default
files, explanatory data and verified archive/bootstrap pins. Native binaries,
setup code, source/artifact evidence, runtime versions, pairings and shader
contracts remain byte-identical. It makes immutable archive/core commits before
refreshing aliases; no GitHub Releases or device operations are performed.
The workflow only runs after an archive push on main/dev. Developer work still
integrates on dev; this does not authorize an agent to merge or publish main.

For a persistent user override, write just a level name followed by a newline
to logging.conf (separate from the owned logging-default.conf):

- PC: %LOCALAPPDATA%/steam-link-native-passthrough/logging.conf
- Frame: ~/.local/share/SteamLinkNativePassthrough/stream-bridge/logging.conf
  (or the corresponding XDG_DATA_HOME root).

SFNP_LOG_LEVEL overrides both files for that process. Values are case-insensitive:
Debug, Important, Warning, Error. Native overrides are read on process startup;
restart the affected runtime yourself to apply a manual override. Malformed,
unknown or oversized files never disable errors or change native admission.
The installer reads its package default, with SFNP_LOG_LEVEL as an override.
Important setup logs omit routine polling/stages but retain start/completion,
errors, cancellation, recovery paths and version plans. Existing redaction,
private permissions and the 256 KiB per-session cap remain unchanged.

This logging update changes both native components. Its first installation may
need the ordinary coordinated Frame update/restart after the user's Apply.
Later PC-only updates with identical Frame files/default retain the existing
no-Frame-writes/no-Frame-restart behavior. Switching channels changes a Frame
configuration default and therefore legitimately needs coordinated application.
No SSH writes or Frame restart were used during development.

Software checks cover actual game/loader sinks, embedded branch defaults,
incremental switches, malformed overrides, original installer upgrade/Undo,
retargeted real ZIP integrity and byte-identical native/setup payloads. A change
of logging level is not a new headset picture certification.
