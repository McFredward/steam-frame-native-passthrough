# Negotiated Steam Link bridge: foundation checkpoint

The maintainer authorized a PC sender companion on 2026-10-18. The intended
replacement for the existing packed atlas keeps ordinary Steam Link RGB and
adds genuine, frame-associated coverage through its video transport. This
checkpoint implements **metadata negotiation and opaque fallback only**. It is
not a new passthrough renderer, a deployed sender, or a sharpness fix.

## Admission and ownership

- HELLO/ACK/COMMIT requires both local and remote commits, fresh session nonces,
  compatible protocol/capabilities and an independently authenticated channel.
  Parsing an owned SEI does not establish authentication.
- The producer preserves the exact borrowed original AU on refusal. An owned
  augmented AU stays alive until its actual consumer finishes borrowing it.
- The receiver always returns the exact original AU pointer and length. Probe
  observation, decoded-frame association and selected-surface association are
  separate evidence. None grants coverage or shader activation.
- No masks, coverage permission, sockets, installed hooks, or image changes are
  provided by these portable adapters. All source/association trust inputs are
  false unless established by a real provider.

## Local validation

The focused CMake gate passes the four protocol, HEVC SEI, sender-lifetime and
receiver-association suites. An independent software-codec fixture encodes four
real HEVC access units with libx265, appends/extracts/removes the owned metadata,
and decodes the original and augmented streams with FFmpeg. Every original NAL
survives byte-for-byte and all four decoded frame hashes and cadence match.
Duplicate insertion is refused without output.

The same portable library and offline AU tool compile strictly for Linux x86-64,
Windows x86-64 and Linux AArch64. Cross-compilation does not establish that a
Steam runtime on those platforms exposes the required interception boundary.
The codec test does not establish Steam Link SEI preservation, hardware decode,
peer authentication, surface selection, or native camera composition.

Run the small isolated gate without building/installing the Frame loader:

```sh
cmake -S . -B build/bridge -DFRAME_STREAM_BRIDGE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/bridge --parallel --target \
  test_stream_bridge_protocol test_hevc_bridge_sei \
  test_stream_bridge_sender test_stream_bridge_receiver stream_bridge_au
ctest --test-dir build/bridge --output-on-failure
```

The codec fixture accepts an explicit FFmpeg path, evidence directory and the
GloomhavenVR checkout providing the common completed-run cleanup API. It retains
small source/capture/result evidence and removes its owned transient cache.

## Remaining deployment boundaries

The host's reported installed Windows driver SHA-256 now matches the privately
reviewed driver. A source-pinned host provider, authenticated pairing/control,
the receiver's actual coded-AU boundary, real same-frame selection and renderer
integration still require their own evidence. A guided installer must refuse
live installation of an incomplete prototype bundle. Do not publish a release
manifest or claim Linux runtime support from the portable compile alone.
