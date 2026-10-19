# Native MR sender backends: AMD and Linux

Status: implementation foundation, 2026-10-19. This does **not** enable AMD or
Linux in the installed sender. No device installation or native Steam change is
part of this work.

## Why the existing Windows/NVIDIA boundary exists

The paired transport, HEVC certificate and Frame receiver are not inherently
NVIDIA-only. The current PC provider is: it pins one Windows VRLink PE and its
loaded executable section, observes its DirectMode009/D3D11 composition, and
intercepts its future public `NvEncodeAPICreateInstance` lookup. Input-resource,
submission and completed-AU ownership are implemented with NVENC structures.
Those facts cannot be inferred merely from the presence of another GPU.

Removing the platform/vendor check would leave AMD without an observed encoder
input/output boundary, and native Linux without the observed D3D11 composition
or Windows source-registration channel. It would not produce working MR.

## Sources actually inspected

| Sender | Retained native evidence | Conclusion and limit |
| --- | --- | --- |
| Windows x64 | VRLink PE SHA256 `1e846be041039798580a91452371f6288c614fb4c152126b137e2c693fb77eef`; actual `AMFInit`, `AMFQueryVersion`, HEVC component creation, DX11 surface creation and `SubmitInput` references | A real AMD AMF/D3D11 path is compiled into this module. This does not establish that an AMD machine runs it, which AMF version it loads, or the lifetime of its output at TX. |
| Native Linux x64 | Official retained VRLink ELF SHA256 `d47f05b90dd29bcac1cfe41d75effbeb5e16a5f960ceb11979af365fedd8ac08`; `SVLHMDDriverVulkan`, `videovulkan`, and public Vulkan encode/query dispatch references | This sender uses Vulkan Video HEVC, not the Windows NVENC boundary. Static dispatch references include real indirect calls, not just unused names. No live Linux sender or GPU has been tested. |

The Linux module checks RADV and NVIDIA driver versions, but those private
version checks are not a compatibility promise from this project. Retained
module/depot pins also do not prove the user's current SteamVR installation.
Private binaries and disassemblies remain outside the repository.

Running Gloomhaven through Proton does not make SteamVR's native Linux sender a
Windows/NVENC process. Its Vulkan composition and encode path needs its own
source and ownership adapter. AMD on Windows and AMD on Linux therefore have
different encoder boundaries.

## Implemented public AMF boundary

`pc/stream-bridge-amf-boundary.{h,cpp}` uses the original AMD SDK interfaces, not
copied vtable layouts or guessed private offsets. Its owner must supply original
typed methods and a proved native HEVC/frame-output contract. The component's
actual HEVC output-mode property is checked; slice output is refused.

For each admitted input, the adapter stores an exact source/content identity and
adds an owned cookie/domain to AMF's custom properties. AMD's own sample
documents copying custom input properties to output. Output matches that cookie
and the fresh attachment domain, never PTS, FIFO order, the latest input, or an
approximate clock. The domain must be a fresh nonce for every attachment,
including DLL reloads; it cannot be reused while old native outputs may exist.

The native `SubmitInput` and `QueryOutput` each execute exactly once with their
original arguments, results and caller output pointer. `AMF_OK` and
`AMF_NEED_MORE_INPUT` accept an input; `AMF_INPUT_FULL` does not. Up to 32 pending
inputs are retained. A query racing ahead of submission acceptance retains its
output until the actual acceptance result is known. Successful Flush retires
old generations; failed Flush does not. Stop/ReInit/Terminate revoke receipts
without suppressing ordinary native forwarding. Concurrent reuse of one pending
AMF data object disables the attachment because delayed property copying could
otherwise attach a new cookie to old pixels.

This foundation only observes a borrowed, reference-held AMF output. It does not
install a hook, append a certificate, replace an AMF buffer, alter pixels,
negotiate a peer or open a device. Input properties are AMF metadata; their actual
runtime cost and propagation still require AMD hardware verification. SDK calls
may allocate internally. The code makes no render-thread allocation claim.

## Remaining production ports

1. **Windows AMD:** pin and observe the actual AMF factory and HEVC component;
   intercept only its public typed methods with process-safe callback lifetime.
   Bind the DX11 input surface to the same observed D3D11 composition/coverage
   generation. Then append a certificate at the exact complete AMF AU before
   the native consumer's TX handoff, preserving native metadata and ownership.
   Deferred observer notification alone does not prove that this handoff can
   still be modified in time.
2. **Linux source:** implement and prove native DirectMode/Vulkan image imports,
   exact game-source registration across Proton/native SteamVR, queue completion
   and the original composition/foveation mapping. Windows shared handles and
   named pipes cannot simply be reused as native Linux Vulkan/Unix identities.
3. **Linux encoded AU:** track the real video session/profile, command-buffer
   recording generation, source image view/subresource and destination buffer
   range from `vkCmdEncodeVideoKHR`; bind the exact query-pool generation/query
   index and successful native submission. Observe native encode-feedback
   offset, byte count and status before the original AU's TX handoff. Buffer
   handles, query indices and command buffers are reusable, not frame IDs.
4. **Both:** validate local composition/coverage, reordered and cancelled
   encoding, resize/reset, complete AU delivery and paired-device continuity on
   actual hardware. Unknown source/session/eye/frame/extent must remain opaque.

The Linux public APIs provide concrete ports, but an encode command/query result
alone does not identify the game scene or authorize coverage. No unknown native
module or unobserved GPU backend is admitted by this work.

## Focused controls

Supply AMD SDK headers from commit
`57ee0cfb0e1a08d9041f77435653794c1b012be0`; the offline runner verifies all 19
transitively included public-header hashes. It never downloads SDKs or installs
anything:

```sh
python3 tests/run_stream_bridge_amf.py \
  --amf-include /private/AMF/amf/public/include \
  --output /private/new-amf-test-run \
  --cross-compiler x86_64-w64-mingw32-g++
```

Add `--retention-checkout /home/claw/gloomhaven_vr` in this workspace to invoke
the shared completion-cleanup API. The exclusively owned native cache is also
removed; commands, source/SDK hashes and results remain. Controls instantiate
the actual SDK interfaces and exercise original forwarding, same-PTS/reordered
outputs, output-before-acceptance, rejection, ambiguity, bounded references,
stale output and lifecycle. The Win64 step is a compile check, not Windows/AMD
execution. These are software interface/lifetime proofs, not a headset result.

## Official API references

- [AMD AMF component API reference](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/57ee0cfb0e1a08d9041f77435653794c1b012be0/amf/doc/AMF_API_Reference.md)
- [AMD HEVC properties](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/57ee0cfb0e1a08d9041f77435653794c1b012be0/amf/public/include/components/VideoEncoderHEVC.h)
- [AMD sample: copied custom input/output properties](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/blob/57ee0cfb0e1a08d9041f77435653794c1b012be0/amf/public/samples/CPPSamples/common/PollingThread.h)
- [Khronos: vkCmdEncodeVideoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdEncodeVideoKHR.html)
- [Khronos: vkGetQueryPoolResults](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)
- [Khronos: encode-feedback flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeFeedbackFlagBitsKHR.html)
