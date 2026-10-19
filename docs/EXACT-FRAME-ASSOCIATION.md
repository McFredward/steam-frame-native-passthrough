# Exact association prerequisite

> **Historical reference.** Historical generic ledger contract. The missing-adapter statement below applies to this foundation, not the current paired native implementation. Current source, AU and selected-image proof is described in [Developing](DEVELOPING.md) and the [coverage contract](protocols/stream-coverage-v1.md). The old ledger alone is not a runtime admission path.

`mask/frame_association.h` adds a bounded scalar ledger beside the mask receiver.
It does not hook Steam Link, extract scene coverage or activate passthrough.
The native adapter is still missing; synthetic tests cannot establish a private
runtime ABI or the picture in a headset.

The adapter must supply this complete chain, from actual native observations:

1. Source session and full 64-bit video-frame identity, with each eye's mask extent.
2. The transmitter's native 16-bit key **and an explicit reuse generation**.
3. The decoder-selected surface, including its namespace and content generation.
4. The compositor's imported resource, including a generation for that content.

A Vulkan allocation can hold many successive video frames. Its allocation
lifetime is insufficient for step 4. `PresentNumber`, packet arrival order,
submission counters and approximate timestamps are not substitutes for any
missing link. The [receiver dataflow](FRAME-ASSOCIATION-DATAFLOW.md) and
[resource-route investigation](FRAME-RESOURCE-ASSOCIATION-20261011.md) identify
separate native boundaries that a future adapter must actually observe.

Mappings may arrive out of order. `resolveExact` returns a `FrameIdentity` only
when the selected session, native key/generation, decoder surface/generation,
imported resource/generation, eye and extent all match. Missing or conflicting
proof means opaque presentation. Repeated presentation may resolve the same
identity again; keep the corresponding GPU mask alive independently of the
receiver's one-shot `takeExact` operation.

Storage is fixed: 64 source frames, 64 native keys, 128 decoder handles and 128
import handles. Proven retirement barriers reclaim source/key entries, while
per-handle generation tombstones prevent delayed evidence from reviving retired
content. Never set a retirement barrier merely from the newest arrival. Capacity
loss or contradictory evidence disables resolution until a fresh session.
Call the ledger serially; it has no allocation, networking, clocks or GPU work.

The independent activation gates remain: selected-frame proof, coherent eye/UV
values, free descriptor/layout binding, GPU lifetime/synchronization and an
opaque fallback. Passing these software state tests does not satisfy those gates.
