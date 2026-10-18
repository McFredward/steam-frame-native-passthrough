# Production process and artifact boundaries

`run.py` exercises the actual process worker cycle and its authenticated local
image-route lifecycle. It does not start the background worker or a Steam process.

`artifacts.py` adds the real installed-file boundary. It compiles
`stream_bridge_process.cpp` and calls its production `readCoverageFragmentCold`
and `readLegacyFragmentCold` functions with locally generated private shaders.
The renderer fixture's in-memory reader stubs cannot establish this evidence.
Exact scalar coverage and legacy files must load; historical array3 coverage,
changed/missing files, wrong permissions, links and partial output must refuse.
A causal control restores the obsolete array3 runtime pin and must fail the
positive scalar-file assertion. The generator test independently compares its
actual output with the authoritative C++ pins and installer pins.

```sh
python3 tests/stream-bridge-process/artifacts.py \
  --libraries /path/to/host-cmake-static-libraries \
  --coverage /path/to/private/scalar-coverage.spv \
  --legacy /path/to/private/legacy-decoder.spv \
  --historical-count3 /path/to/private/array3-coverage.spv \
  --output /path/to/private/evidence
```

The runner checks normal and ASan/UBSan builds, synchronously reaps every child
and removes its temporary compiler tree, preserving results and source/input
hashes. No native shaders, generated native code or headset image is published.
Passing file admission is distinct from a valid native pipeline, video frame or
correct hardware picture.
