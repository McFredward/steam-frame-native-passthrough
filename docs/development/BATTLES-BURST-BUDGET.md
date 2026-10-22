# Battles transport resets and burst accounting, October22

## Captured result

Demeo Battles1.4.21 uses PC0.2.16 on SteamVR2.18.4. Public binding discovery
succeeds, the v2 producer requests rate accounting, native outputs match exactly
(10,617 exact, zero unmatched), and 5,380 full-resolution masks are certified.
The Frame receives 5,368 masks; its decoder handoffs report zero emergency
handoff timeouts. Game recenter guards run during avatar/scenario setup; the
captured PC compositor has no new standing-origin reset during that transition.

The two reported blackouts have matching native transport failures:

| Frame time | Evidence |
|---|---|
| 08:32:19.442 | 228-packet substantive retry for frame7858, expected AU280,525bytes |
| 08:32:19.487 | Outside backtrack window; unrecoverable FEC error; late frame7863 metadata |
| 08:32:19.529 PC | Native encoder reset3; paired source transport retires |
| 08:32:21.186 | Further180/49-packet retries |
| 08:32:21.242 | Second outside-window/unrecoverable error, late frame8016 metadata |
| 08:32:21.286–21.475 PC | Consecutive native resets4–6, then authenticated source recovers |

The ordinary startup decoder reset at08:30:50 precedes game passthrough and is
separate. Repeated retries also occur before the game adds masks. No captured
fact proves that masks or WLAN alone cause every lost packet. The failing
280,525-byte AU itself is below the nominal per-frame bitrate allowance. These
logs establish transport loss and recovery, not a complete physical link model.

The twelve-file, hashed read-only capture is retained privately as
`frame-battles-blackouts-20261022-063528` in the Demeo debug directory. The Frame
was not modified. Aggregate mask/video counters span different image content;
331,069,806 extra mask bytes accompany729,181,273 original video bytes in this
run. This is not a controlled compression benchmark.

## Concrete rate-policy defect and PC0.2.17 correction

The prior policy subtracts a32-output average of metadata from NVENC average/max
video rates, waits for eight samples and leaves original VBV size/delay intact.
It therefore does not constrain the combined video-plus-coverage burst. A
loading-scene spike is diluted by small earlier masks; an explicitly configured
video buffer can still authorize an original-sized video frame with coverage
added afterward. Zero/default VBV values do not establish a bounded buffer.
The old logs did not record live VBV fields, so this is a reproduced source-level
budget defect and a plausible contributor, not proof of its exact live magnitude.

For the existing explicitly requested v2 policy only:

- Reserve the largest actual escaped metadata payload in a bounded128-output
  window, beginning with the first current-source sample. A new peak attacks
  immediately; release waits for128 subsequent outputs. Authority/encoder
  lifetime changes still discard measurements. Rate history never grants image
  or camera authority and never substitutes another frame's coverage.
- Scale explicit native VBV size/delay by the applied/requested video rate and
  cap the buffer at one applied-video frame. Keep stricter native buffers and
  clamp delay to the new size. Zero/default fields become explicit bounded
  values. The existing source-contract comparison already classifies these
  public RC fields as mutable QoS fields.
- Deep-copy temporary native arguments. Keep the original requested config for
  subsequent NULL-config QoS and policy withdrawal. Return the exact native
  status without retrying, resetting, changing resolution/FPS or hiding refusal.
- Cold five-second diagnostics record reserved peak/mean metadata rates,
  requested/passed VBV size/delay and pressure above the retained video floor.

NVIDIA recommends small, approximately single-frame VBV buffers for low-latency
game streaming in its [NVENC programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html#recommended-nvenc-settings).
The implementation uses only public fields from the independently pinned SDK11.1
header; it does not infer a percentage meaning for Steam Link's logged FEC32.
No undocumented FEC, retransmission or decoder timeout setting is changed.

The25% video-rate floor remains. A mask load that alone exceeds the remaining
budget cannot be made to fit by this policy; pressure is reported rather than
removing masks or claiming success. Video quality may decrease under peak load.
No software bitrate policy can guarantee against arbitrary network packet loss.
The follow-up below records the subsequent headset test at automatic bitrate.

## Software witnesses and scope

The actual production NVENC callback fixture first fails the new burst assertion
with the old implementation. With PC0.2.17 it checks the native forwarded VBV,
first-sample/peak reservation, original argument immutability, stricter/default
buffers, unchanged return status/source generation and full original restoration.
A100Mbit/s,120Hz example with70,000 added bytes allows273,333 video-buffer bits;
video buffer plus metadata fits the original833,333-bit frame budget. Leaving
the original1,000,000-bit video buffer permits an extra burst despite the correct
32.8Mbit/s video average.

Three independent causal controls restore only the old unaccounted rate,
unbounded VBV, or smoothed metadata estimate. Each must fail its corresponding
production callback assertion. Linux/Windows tests preserve the v1/Gloomhaven
original native pointer/rate/VBV and current frame/eye/source ownership behavior.
They do not run a physical NVIDIA encoder or certify zero headset blackouts.

This is a **PC-only** patch update: PC0.2.17, unchanged Frame0.2.13 and unchanged
installer0.2.16. Every Frame payload/helper/settings target remains byte-identical
in the new archive. Matching Frame installations require no writes or restart.
Game mod1.4.21, native update recognition, lossless mask encoding, public wire
protocol, shader output and Gloomhaven's v1 policy are unchanged.

## Confirmed follow-up, October 22

The maintainer reports no visible problems in the follow-up with PC 0.2.17,
Frame 0.2.13 and Battles 1.4.21. PC SteamVR is **2.18.4, native build
1792185270**; Frame SteamVR is **2.18.2, native build 1791331621**. These versions
are now officially supported, successfully tested observations, not an activation
allowlist for future updates. PC 2.17.10 remains a historical successful test.

The private read-only capture `frame-battles-verified-20261022-072002` preserves
all twelve hashed log files. Frame runtime fingerprints were re-read separately;
all eleven match the previous reviewed Frame references. No device was changed.

- The game enables native RGBA at 09:16:58.362. Scenario setup at 09:17:21
  suppresses three automatic avatar teleports; the earlier avatar reset is also
  suppressed. Last game diagnostics show equal eye submit counts, zero submit
  errors, zero origin notifications and zero discontinuous pairs.
- The sender finishes with 10,330 exact outputs and zero unmatched outputs;
  8,719 masks are emitted. Frame decoded masks rise from 5,368 before this run
  to 14,087 after it: exactly 8,719 additional masks, with zero rejected masks
  or uploads. These are counter deltas, not claims about historical cumulative
  totals. All 8,790 budgeted reconfigurations are accepted; zero refusals or
  over-budget reports occur. Encoder-generation revocations stay at the four
  startup events rather than growing during the game.
- No new native reset, unrecoverable FEC error or backtrack-window failure occurs
  after native passthrough starts. The decoder hard reset at 09:16:41 is initial
  stream setup, before the game starts. Shutdown at 09:18:37 is explicit server
  shutdown. Neither is counted as a gameplay outage.
- Recoverable packet retries persist. At 09:18:31.451 native Steam Link logs one
  submitted frame without metadata followed by late metadata for frame 9858.
  No stream reset follows. The compositor's opaque-submit counter stays at
  39,117 from 09:17:06 through 09:18:36, with no new mask/submit refusals or
  pipeline loss. The maintainer did not see a flash. This is a residual transport
  warning, not evidence that the network is lossless or every future run is safe.
- The supplied game ErrorLog contains only Steam minidump initialization lines,
  not a game exception or crash. Existing loader/driver startup warnings are
  distinct from the previous paired transport failures.

Installer 0.2.17 records the current PC driver fingerprint and recognizes the
confirmed semantic/native version pair, while retaining the prior version label
and the warning-and-enable path for untested updates. PC patch 0.2.17 and Frame
patch 0.2.13 are byte-identical to the headset-tested package. This is installer
metadata only; an already-current installation needs no native update or Frame
restart. Other games and long-duration/network conditions retain their own test
scope; this Battles result is not a new Gloomhaven or Battlemarked headset test.

### Fixed-bitrate software witness

The production callback fixture also holds both requested CBR average and
maximum at 100 Mbit/s while the exact added mask payload changes through
3,000, 20,000, 60,000 and 3,000 bytes. With the first current-source 7,000-byte
sample still in history, the forwarded video rates are 93.28, 80.8, 42.4 and
42.4 Mbit/s. No Auto target change is needed. Each step checks both the combined
rate and one-frame VBV budget, unchanged caller config and retained exact
source/coverage authority. No production change is needed for this case; the
headset-tested native DLL is retained byte-for-byte. Actual NVENC callback
availability and physical link behavior at a fixed setting still need their
own headset run; this software witness does not simulate Valve's full rate
controller or every possible network load.
