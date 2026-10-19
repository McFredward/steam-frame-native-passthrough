# Linux sender: Vulkan Video implementation plan

Linux sender installation remains unsupported. The new public-API completion
boundary is a first implementation component, not a working Linux passthrough
backend. No installer compatibility flag or existing Windows path changes.

## Established evidence

A read-only reference obtained from the official Steam depot identifies Linux
x86_64 `drivers/vrlink/bin/linux64/driver_vrlink.so` with SHA256
`d47f05b90dd29bcac1cfe41d75effbeb5e16a5f960ceb11979af365fedd8ac08`.
Its public loader/dispatch references include `vkCmdEncodeVideoKHR` and
`vkGetQueryPoolResults`. The inspected query caller requests one 12-byte
32-bit offset/byte-count/status record, with WAIT and WITH_STATUS flags. These
are static reference facts, not evidence of an active Linux stream, a completed
AU consumer lease or the runtime version on a user's computer. Valve binaries
and private disassemblies are not distributed with this project.

Khronos specifies that an encode operation identifies its input picture view,
subresource and destination bitstream range. Its feedback can identify the
completed bitstream offset and byte count; successful result status matters.
An already available query result alone cannot establish that a newly recorded
reset/encode has executed. The implementation additionally requires successful
completion of the actual submission fence before yielding the current record.
See [video encode commands and feedback](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_video_encode_queue.html)
and [query results and status](https://docs.vulkan.org/spec/latest/chapters/queries.html).

The reference also contains `VK_VALVE_video_encode_rgb_conversion`. That public
extension permits RGB/RGBA encode inputs. It offers a possible direct alpha
capture boundary, without reconstructing coverage from YUV or key colours.
Presence of the extension name does not prove that this profile was selected or
that alpha survives native compositing. Its feature, profile and actual image
format must be observed together. See the
[extension specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_VALVE_video_encode_rgb_conversion.html).

## Implemented boundary

`pc/stream-bridge-sender-linux-video.{h,cpp}` forwards public Vulkan ABI calls via
an explicitly supplied original dispatch. It is not yet wired into a loader
layer or provider. Its bounded per-device ledger records successful native HEVC
session, query-pool and image-view creation; command recording/reset/encode;
actual queue submission; actual fence status; and native encode feedback.

A yielded `LinuxEncodedRegion` contains the original image/view/format,
subresource, coded rectangle, HEVC picture order count, exact destination buffer
region and lifecycle generations. It is emitted once. Unknown, failed,
unsupported, stale, reused or ambiguous records remain unproven. Native calls,
arguments and return values remain unchanged. The component adds no Vulkan
polls, GPU waits, file/network operations, memory mappings or bitstream writes.
Image/view/session/query destruction and fence reset revoke pending records.

The first scope supports explicitly reset ordinary queries, a single encode
operation per command/query, original submissions with a tracked non-null fence and
32/64-bit offset/byte-count/status feedback. Inline queries, Submit2, cached
repeated submission, missing native fence observations and extended feedback
layouts require further implementation. Each tracked object table has 32 fixed
entries; larger native configurations require an explicitly reviewed capacity change. Refusal is not permission to guess.

A completed encoded **region** is not yet a borrowed complete-AU lease, source
registration, coverage certificate, or peer authorization. In particular its
image handle is evidence, not an ownership reference that permits later GPU
access after the native owner destroys it.

## Remaining production work

1. Add an owned Vulkan loader layer that forwards the complete required ABI,
   including extension aliases, dispatchable-object lifetimes and native
   synchronization. Bind it only to the reviewed native Linux driver/device.
   No multibyte executable-code patch or replacement of native encoding is
   needed for this approach.
2. Observe exact source images before native RGB conversion, record layouts,
   queue-family ownership and transitions, and capture alpha in the same GPU
   dependency chain. If the selected encoder input is already YUV, establish
   the native RGB conversion input rather than inventing alpha afterwards.
3. Track buffer creation, bound memory, mapping/coherency and the actual native
   bitstream consumer. Augmentation must preserve the original complete AU and
   native bytes, remain within legitimate capacity or use a proven consumer
   lease, and retain owned bytes until the consumer releases them. Query byte
   count alone does not establish those permissions.
4. Implement Linux source-format registration across the actual Proton/Wine
   application boundary. Linux peer credentials identify the kernel socket
   peer; a Wine process ID or an arbitrary relay PID must not be treated as
   the application's verified kernel identity. Keep registration, source-image
   provenance, per-eye coverage and exact AU association separate.
5. Bind the backend to the shared `SenderBackend`/`SenderRuntime` port, retaining
   authenticated pairing, fresh-session recovery, exact certificates and
   source revocation. The existing packet protocol can be reused; this first
   component does not introduce another network transport.
6. Add actual Linux encode/readback/consumer tests on a driver exposing the
   selected HEVC profile, negative codec/source/lifetime controls and paired
   headset verification. Only then package a Linux provider and extend the
   installer's exact platform/runtime inventory.

## Focused software checks

The new fixture calls the production boundary through real public Vulkan
function types with an explicit in-process native API fake. It checks native
forwarding, exact input/range provenance, 32/64-bit results, duplicate refusal,
old-query-before-fence refusal, native failures, generation reuse, destruction,
and bounds. It does not run a Vulkan implementation or claim GPU encoding.

```bash
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -I. \
  pc/stream-bridge-sender-linux-video.cpp \
  tests/test_stream_bridge_sender_linux_video.cpp -pthread \
  -o /tmp/sfnp-linux-video-test
/tmp/sfnp-linux-video-test
```

A first implementation checkpoint must therefore be described as a verified
public-ABI foundation with explicit remaining gates, not as Linux support.
