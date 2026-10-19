# Repository installer payload

`steam-link-native-passthrough.zip` is the validated paired installer package.
It includes owned PC/Frame companions and the pinned setup dependencies, not
Valve binaries, private shaders, pairing secrets or hardware captures.

No GitHub Release is needed to deliver routine candidates:

1. Build and validate the exact source/artifact-bound package with
   `tools/package_stream_bridge.py`; keep the software evidence separately.
2. Commit the completed ZIP here. Do not bypass its manifest or validation gates.
3. Pin that immutable full commit URL, exact byte size and SHA256 in `get.ps1`
   and `get.sh`. Run the focused bootstrap tests, including the ordinary entry.
4. Update the compatibility aliases to the new bootstrap commit and hashes.
   Publish the validated source and payload to `main`.

The archive's own source revision describes its native/setup contents; a later
bootstrap-only commit does not require rebuilding unchanged native components.
An archive mismatch aborts before extraction. Updating an installed device still
requires the assistant's inspection and explicit Apply confirmation.
