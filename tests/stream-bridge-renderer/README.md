# Actual renderer integration fixture

This focused fixture compiles the production `stream_bridge_renderer.cpp` and
`stream_bridge_coverage_gpu.cpp`. It exercises public Renderer entry points,
its real definition journal and real mask pool. The public Vulkan fake executes
the renderer's recorded immutable GPU guard writes in queue order and observes
the value read by the original native command. It does not copy the renderer's
admission decisions into a second implementation.

Coverage includes flags=0 replay, current and expired image leases, source EOF,
unknown/reset commands, simultaneous-use refusal, descriptor/view generations,
actual upload completion, failed original submits, retirement-fence capacity,
independent shared-UBO offsets, and both core and KHR submit dispatch. A batch of
129 original submit infos checks exact order/pNext forwarding and a single fence
on the final bounded chunk. Two semantic controls remove current-lease checking
or the leading zero command; both must fail a positive/negative assertion.

External process admission, authenticated picture callbacks and lease validity
are explicitly supplied fixture observations. A regular temporary fd tests the
renderer's kernel-object join, not an actual DMA_BUF export. This is not a
Steam/Frame, full decoder, physical-eye or GPU-picture test. Existing actual R8,
fragment and codec proofs remain separate.

Run from any checkout with explicit private shader inputs (never commit them):

```sh
python3 tests/stream-bridge-renderer/run.py \
  --native-root /path/to/native-checkout \
  --output /path/to/private/run-renderer \
  --vs /path/to/original-vertex.spv \
  --ps /path/to/original-fragment.spv \
  --coverage /path/to/local-coverage.spv \
  --legacy /path/to/local-legacy.spv \
  --sanitize --mutants
```

The runner takes immutable source snapshots before compiling and records their
hashes with the private input hashes. It reuses existing structural/fake-API
builders without executing their unrelated test mains. Results belong under the
main checkout's ignored debug directory. After all child processes are reaped,
run completion cleanup and remove only this run's regenerable `cache/`; retain
its source snapshots, logs and results.
