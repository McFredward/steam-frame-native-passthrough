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
definitions before the companion can register a route. A control removes
that authorization and must fail. Merely observing count1, or supplying a
mutated original fragment, cannot promote it. The older count3 integration cases
remain unchanged. The generated coverage module defaults to an independently
specialized count1 interface, also usable with a larger count3 layout.

The native creation callback preserves the original pipeline handle and picture.
It creates only a separate exact-coverage companion. Only a verified current
image lease selects that companion at an eligible draw; missing coverage,
unsupported recordings, lost observations and failed preparation keep ordinary
native RGB. There is no picture/mask-atlas decoder or legacy pipeline fallback.
Coverage and layout retirement destroy only owned objects, leaving the caller's
native pipeline to its original destroy. A control that changes the public
native handle must fail. Custom allocation callbacks remain unsupported and
keep their original object untouched.

`--artifact-fallback-only` checks unavailable coverage, unchanged native pipeline
and exact original-submit forwarding, plus a valid scalar-coverage control. The
separate `tests/stream-bridge-process/artifacts.py` fixture uses the actual cold
file reader and verifies that an obsolete atlas file cannot replace missing
coverage. The renderer fixture's supplied artifact observations do not prove
installed-file admission.

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
  --sanitize --mutants
```

The runner takes immutable source snapshots before compiling and records their
hashes with the private input hashes. It reuses existing structural/fake-API
builders without executing their unrelated test mains. Results belong under the
main checkout's ignored debug directory. After all child processes are reaped,
run completion cleanup and remove only this run's regenerable `cache/`; retain
its source snapshots, logs and results.
