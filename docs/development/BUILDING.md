# Build and validate the current paired route

This is a developer workflow, not an installation guide. Players use the
[guided installer](../../README.md#installation). A local build never authorizes
a device write or service restart.

For a game-side integration, start with the [game guide](GAME-INTEGRATION.md)
and its affected-check matrix. Reusing the existing application-source API does
not itself require rebuilding or reinstalling the native companions.

## Portable modules and focused tests

The commands below require CMake/CTest 3.20+, C++17 and Python 3.
`FRAME_STREAM_BRIDGE_ONLY=ON` selects the
current bridge without building the older diagnostic/atlas targets. For example:

```sh
cmake -S . -B build/bridge -DFRAME_STREAM_BRIDGE_ONLY=ON \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/bridge --parallel 4 --target \
  test_stream_bridge_frame test_stream_bridge_compression \
  test_stream_bridge_decode_cost
ctest --test-dir build/bridge -C Release --output-on-failure \
  -R '^stream_bridge_(frame|compression|decode_cost)$'
```

Linux also provides `test_stream_bridge_control`, `test_stream_bridge_local`,
`test_stream_bridge_native_decode` and `test_stream_bridge_native_runtime`.
Build the actual targets before selecting their names with `ctest -R`.
For the game-source wire format and native source ownership boundary:

```sh
cmake --build build/bridge --parallel 4 --target \
  test_stream_bridge_source_contract test_stream_bridge_sender_provider_sourceipc
ctest --test-dir build/bridge -C Release --output-on-failure \
  -R '^stream_bridge_(source_contract|sender_provider_sourceipc)$'
```

Native Process, renderer and real Vulkan fixtures have additional explicit
inputs; use their [Process](../../tests/stream-bridge-process/README.md),
[renderer](../../tests/stream-bridge-renderer/README.md) and GPU runner instructions.
Private shader/runtime references are inputs, never public distribution members.

Choose the tests for the changed production boundary. Run its causal failure
controls and sanitizer coverage when relevant. A portable test is not a Windows
driver test; Wine is not a substitute for the actual Windows D3D/WARP suites.
The workflows under [`.github/workflows`](../../.github/workflows) specify the
SDK pins, platform dependencies and current CI commands. Reuse already-passing
evidence for unchanged areas after a bounded repair; report that scope honestly.

## Windows sender companion

The reproducible cross-build uses MinGW x86_64 on Linux and two **explicit,
reviewed public headers**, `nvEncodeAPI.h` and `openvr_driver.h`. Their exact
SHA256 values are enforced by
[`stream-bridge-sender-provider-build.sh`](../../pc/stream-bridge-sender-provider-build.sh);
the pinned upstream URLs are in the native/lifecycle CI workflows. Configuration
does not download them or accept a newer SDK silently.

```sh
bash pc/stream-bridge-sender-provider-build.sh \
  /absolute/path/to/reviewed-sdk "$PWD" /absolute/path/to/sender-output
```

This produces `driver_steam_link_native_passthrough.dll` and
`driver.vrdrivermanifest`; it does not register or load the driver. The SDK headers
and a successful PE build do not add Linux/AMD/Intel runtime support. To enable
actual portable sender-boundary fixtures, pass
`-DSFNP_NVENC_SDK_DIRECTORY=/absolute/path/to/reviewed-sdk` during configuration.

## Frame ARM64 receiver and selector

On a Linux build host with the reviewed AArch64 GNU toolchain and Vulkan headers:

```sh
cmake -S . -B build/frame -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DCMAKE_ASM_COMPILER=aarch64-linux-gnu-gcc \
  -DFRAME_STREAM_BRIDGE_ONLY=ON -DFRAME_STREAM_BRIDGE_RECEIVER=ON \
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/frame --parallel 4 --target \
  VkLayer_steam_link_bridge sfnp_stream_launch
```

Outputs are `libVkLayer_steam_link_bridge.so` and `libsfnp_stream_launch.so`.
Inspect their architecture, dependencies and exported ABI; retain the private
C++ runtime/linker isolation required by `StreamBridgeReceiver.cmake`. A host
x86_64 build is useful for tests but cannot be installed as the Frame binary.
Do not enable `FRAME_PIXEL_RELEASE_RECEIVER`: the old atlas receiver is retired.
Generation of the admitted coverage shader happens locally from the user's
exact checked runtime, not by distributing Valve-derived shader files.

## Package, publish and retain evidence

Increment [`VERSION`](../../VERSION) for installer changes and the relevant
component in [`PATCH_VERSIONS.json`](../../PATCH_VERSIONS.json) for native patch changes.
The native components, installer source, SDK/tools and dependencies need exact
hashes in the runtime profile. `tools/package_stream_bridge.py` defaults to an
uninstallable prototype. Release admission requires a source/artifact-bound
validation receipt for each required boundary; do not fabricate hardware success
to satisfy a software field. Use `--help` for the current required arguments.

Follow [versioning](../PATCH-VERSIONING.md) and the
[immutable payload publication sequence](../../distribution/README.md): commit
the validated archive, then pin its full commit URL/size/hash in both bootstraps,
then update compatibility aliases. Check the ordinary entry, not only a preview
or an extracted local package. No separate GitHub Release is required. A stable
release can also have a GitHub Release when explicitly requested by the
maintainer; it does not change the immutable installer download route.

Package defaults are dev/Debug and main/Important; they are not new native
versions. `tools/publish_logging_channel.py` checks that retargeting changes only
logging data and integrity metadata, preserving native/setup binaries exactly.
Its `--publish` mode commits and pushes: use it only for authorized publication,
not a game agent's local build. A PC-only update with unchanged Frame payload
must not write to or restart the Frame; see `tests/test_pc_only_setup.py`.

Retain source revisions, exact artifacts, commands/results and borrowed-input
pins. Once child processes have exited, remove generated object trees and
dependency caches that can be recreated. Preserve hardware logs, private inputs,
current deliverables and another worker's active files. Never purge shared
`/tmp` or private runtime references as a shortcut.
