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

The documented native 29-binding scalar Set0 is replayed with the exact private
original shaders. The production successful-shader callback must prove the
original fragment's constant-zero image accesses and promote pending count1
definitions before either companion can register a route. A control removes
that authorization and must fail. Merely observing count1, or supplying a
mutated original fragment, cannot promote it. The older count3 integration cases
remain unchanged. The generated coverage module defaults to an independently
specialized count1 interface, also usable with a larger count3 layout.

The creation callback publishes the already verified self-detecting legacy
decoder as the application's baseline pipeline. The original native object is
private and retained only until baseline retirement. Coverage can temporarily
replace that baseline, then restore it. Losing command observation, an epoch,
or a mask never requires a hot callback to reconstruct the legacy baseline.
Publication, unknown/lost observation, coverage creation failure and exactly-once
public/private pipeline retirement have focused controls; a raw-native baseline
mutant must fail. Custom allocation callbacks remain unsupported and keep their
original object untouched. No caller-owned callback context is retained.

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
