# Pinned LZ4 block codec

Pristine `lib/lz4.c`, `lib/lz4.h`, and `lib/LICENSE` from
[LZ4 1.10.0](https://github.com/lz4/lz4/tree/v1.10.0/lib), commit
`ebb370ca83af193212df4dcbadcc5d87bc0de2f0`. These library files use the
included BSD 2-Clause license; the upstream command-line tools are not included.

| File | SHA-256 |
| --- | --- |
| `lz4.c` | `9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539` |
| `lz4.h` | `26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f` |
| `LICENSE` | `8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c` |

`bridge/stream_bridge_compression.cpp` includes the implementation with internal
function visibility and heap-backed LZ4 state disabled. It calls only the bounded
independent block compressor and `LZ4_decompress_safe`. No streaming dictionary,
cross-frame history, file format parser, upstream programs or build-time download
is used. Preserve this license in binary distribution materials.
