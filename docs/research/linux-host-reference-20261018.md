# Official host references for guided setup

These are private source-review inputs, not redistributable payloads or a Linux
runtime-support claim. They were obtained anonymously through Steam's official
content service with an exact file list; denied optional depots were skipped.
No Valve file was executed or installed. Source manifests, fingerprints and
transfer/cleanup records are retained privately with the development evidence.

| File | Official depot / manifest | Bytes | SHA-256 |
|---|---|---:|---|
| Linux `drivers/vrlink/bin/linux64/driver_vrlink.so` | 250823 / 3404320976981930661 | 2199408 | `d47f05b90dd29bcac1cfe41d75effbeb5e16a5f960ceb11979af365fedd8ac08` |
| Linux `bin/linux64/vrpathreg` | 250823 / 3404320976981930661 | 651032 | `4aa26be9c9203c700b44b216e0c7bbed9dfe1e78fc80a318d09790d529b49720` |
| Windows `bin/win64/vrpathreg.exe` | 250821 / 2377403218982408631 | 840856 | `8a9c2fbc591b9ef65393d26c1b21f6ef1bfce055d7a5d9f94ee539b0cddbf4dc` |

The same official Windows content revision supplies the exact driver hash
reported by the maintainer:
`1e846be041039798580a91452371f6288c614fb4c152126b137e2c693fb77eef`.
That establishes source provenance for the registration-tool pin, not identity
with every user's installed tool. Setup must still check the actual file before
invoking it, and must not bundle a Valve executable.

The common depot is 250824 / 6143398724116101648. Its `bin/version.txt` contains
the bytes `1789498647\r\n`; do not turn this value into an inferred semantic
SteamVR version.

## Linux is a different encoder boundary

The actual Linux x86-64 driver loads Vulkan and resolves Vulkan Video entry
points. Static caller evidence includes `vkCmdEncodeVideoKHR` and
`vkGetQueryPoolResults`, followed by composition of a result offset with a
mapped bitstream base. This is a real encode/result path; a safe complete-AU
borrow interval, original transmit handoff and live interception still need
independent proof. It is not the reviewed Windows NVENC function-table path.

Do not port Windows IAT slots, private RVAs, NVENC ABI assumptions or source
authority onto this ELF. The portable protocol/SEI/authentication modules can
compile on Linux while the actual Linux provider remains unimplemented.
The setup assistant therefore supports Linux inventory/planning and exact
platform-specific package/registration-tool validation, but still refuses
Linux native-provider activation. Proton running the Windows game does not
change the native Linux SteamVR sender into the Windows provider.

The reference acquisition kept fewer than 100 MB of downloads, including the
temporary tooling. Tool/archive/chunk/key caches were removed after their
processes exited; only source references, manifests and small results remain.
