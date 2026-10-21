# Native update recognition, October21

## Observed failure and evidence boundary

The supplied PC SteamVR2.18.4/build1792185270 logs refuse sender attachment
before source permission or either eye's mask can become operational. The prior
PC2.17.10 session attached successfully. Read-only Frame captures still identify
SteamVR2.18.2; their lifetime mask totals do not prove new delivery during the
failed session. This is a PC bootstrap refusal, not renewed game alpha failure.

Private inputs, never redistributed:

| PC driver | Bytes | SHA256 |
|---|---:|---|
| Prior2.17.10 | 13,908,632 | `1e846be041039798580a91452371f6288c614fb4c152126b137e2c693fb77eef` |
| Supplied2.18.4 | 13,928,600 | `8e7100b6b1e88d3ad6dd05f30f030dc9a69b092b64a60d35347cebb5c516b55d` |

The original driver is retained for comparison. These digests are observations,
not an executable allowlist. The new file's signer certificate names Valve Corp.
The Windows runtime independently verifies Authenticode trust and the publisher
on the held file handle; extracting a certificate offline is not that trust test.

## PC recognition

The original tested image keeps its unchanged legacy recognition route. Other
images use bounded PE32+/x64 import and public MSVC RTTI discovery:

- A unique `GetProcAddress` import and the expected live operating-system target.
- Public `IVRDriverDirectModeComponent_009`, its D3D11 implementation, associated
  tracked-device/display bases, and a unique eight-method executable table.
- Current mapped executable bytes equal the currently verified native file.
  Moved sections/methods do not need new expected hashes or offsets. Executable
  base relocations are not currently normalized; a future such image is refused.
- Public NVENC function-list version and all required function pointers still
  validate before callback publication. Observed module identity is immutable
  after that publication.

For adaptive images, luma/chroma selection uses the actual NV12/P010 planar RTV.
The selected SRV's live shared allocation must uniquely match a current submitted
DirectMode layer/eye, held-open allocation, PID, shape and generation. No private
constant-buffer eye offset is assumed. The extra R8 transmittance draw retains
the actual native PS, geometry, UV, constants, sampling and viewport. Native RGB
is forwarded unchanged. Exact input-copy/accepted-encode/output receipts and
coverage authority remain mandatory; no latest-mask fallback is added.

## Frame recognition

The actual AArch64 PIE executable, native role/path and runtime root select the
Frame endpoints. ELF program and dynamic tables discover `mmap`, `munmap`,
`close` and `ioctl` imports. Loaded executable ranges are checked against the
current file; mapped import slots and original glibc/PLT ownership are checked
before atomic hook publication. Full-RELRO read-only import slots are currently
refused. The original verified image retains the legacy exact-callsite route.

For other verified images, actual public V4L2 request/type/codec/memory/planes,
queried allocation, successful mapping/queue/dequeue, timestamp, generations and
kernel DMA-BUF identity prove ownership. A caller inside native code is only an
initial filter, never sufficient capture authority. SO_PEERCRED UID/PID and the
expected same-runtime ELF executable authenticate the local peer. Explicit test
pins retain strict hash semantics.

New camera configs request `FnTable:IVRSettings_003`. A changed owned AArch64
provider can load only after public ELF checks; initialization must return the
exact versioned settings API with required functions. Existing camera lease,
runtime generation, settings ownership and restore rules still govern writes.
Historical configs without this explicit interface policy keep exact pins.

Shader normalization accepts changed compiler-generator metadata, IDs and debug
instructions only if grammar-defined ID operands normalize back to the exact
reviewed executable module. Literal numbers, instructions, descriptor/interface
bindings and arithmetic cannot change. Numeric declaration metadata is derived
from the public [Khronos SPIR-V grammar](https://github.com/KhronosGroup/SPIRV-Headers/blob/main/include/spirv/unified1/spirv.core.grammar.json).
No Valve shader instructions, source, binary or disassembly is distributed.
Actual pipeline/descriptor/current-video proof remains required after recognition.
A genuine shader semantic change still requires a new reviewed transform.

## Software evidence and remaining headset test

The production PE parser discovers the correct import and all eight methods in
both actual supplied PC files. Synthetic PE/ELF controls exercise moved bindings,
wrong architecture, missing/ambiguous imports/tables, a widened interface, invalid
bounds and writable code. Both native V4L2 lifecycle suites run, including a
changed image identity with every admitted callsite moved; failed queue/dequeue,
reuse, source reset and exact handoff tests remain required. Existing local
transport, receiver ownership, camera and installer controls remain in place.

Private real vertex/fragment inputs with every ID renumbered, changed generator
and valid extra debug names normalize byte-for-byte to their originals in both
Python and C++. SPIR-V validation accepts those mutated inputs. An executed
arithmetic change is rejected. The generated coverage shader remains byte-for-byte
unchanged. Public tests use only own synthetic instructions and check that literal
values cannot be mistaken for IDs.

Windows CI runs the production context observer in both modes and the actual
D3D11 alpha companion, including planar target recognition when WARP supplies
those formats. This does not emulate NVIDIA NVENC or certify a native Steam Link
picture. PC cross-compilation and Frame ARM64 builds are software evidence only.
**SteamVR2.18.4 is not a successful headset test until the maintainer confirms it.**

Installer/PC0.2.16 and Frame0.2.13 carry these actual component changes. This
update therefore needs the coordinated Frame component update. Unchanged Frame
components in later PC-only updates continue to bypass Frame file/settings/service
work. PC SteamVR stays closed after setup. An accepted untested-build warning
enables both intended components; runtime-version/file-hash mismatch alone does
not install an inactive patch. Removal and exact ownership recovery are retained.
