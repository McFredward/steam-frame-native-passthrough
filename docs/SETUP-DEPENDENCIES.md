# Portable setup dependencies

The setup bootstrap may provide its own missing interpreter/client. It does not
install a Windows optional feature, SSH service, system Python, pip package or
administrator-level runtime. Dependency availability is separate from the
reviewed sender/receiver activation contract; these dependencies do not turn the
current bridge prototype into an installable release.

## Windows OpenSSH fallback

Prefer the existing Windows `System32/OpenSSH/ssh.exe` client. If it is missing,
the reviewed fallback comes from Microsoft's official
[Win32-OpenSSH 10.0.0.0p2-Preview release](https://github.com/PowerShell/Win32-OpenSSH/releases/tag/10.0.0.0p2-Preview).
**Upstream calls this a preview/non-production-ready release.** GitHub marks it
as the latest non-prerelease, but that flag must not be described as a stable
support guarantee. The fallback choice is explicit in the source manifest.

`packaging/setup-openssh.json` pins the exact 5,704,583-byte ZIP and all four
published files. `windows_openssh(raw, pin)` in
`tools/package_setup_dependencies.py` returns only these bytes:

- `setup/runtime/openssh/ssh.exe`
- `setup/runtime/openssh/libcrypto.dll`
- `setup/runtime/openssh/LICENSE.txt` and `NOTICE.txt`, complete and unchanged
- `setup/runtime/openssh/source.json`, recording the checked upstream source

The helper performs no download, disk extraction, process launch or install.
It validates the whole source ZIP hash, bounded paths/counts/sizes, duplicate
and link rejection, selected file hashes and x64 PE headers. Server programs,
service installers, agent/key tools and PowerShell modules stay unselected.
PE import review of the pinned `ssh.exe` found `libcrypto.dll` plus Windows
`USERENV`, `CRYPT32`, `WS2_32`, `Secur32`, `KERNEL32`, `USER32`, `ADVAPI32`.
The pinned `libcrypto.dll` imports only Windows `WS2_32`, `ntdll`, `bcrypt` and
`KERNEL32`; neither imports a separate Visual C++ runtime DLL. This is static
dependency evidence, not a Windows execution test. Optional PKCS#11/security-key
helper programs are outside this minimal password/ordinary-key client subset;
no service or credential store is installed.

## Linux portable Python

`packaging/setup-linux-python.json` pins Astral's
[20261013 upstream release](https://github.com/astral-sh/python-build-standalone/releases/tag/20261013):
CPython 3.13.16, baseline `x86_64-unknown-linux-gnu` and
`aarch64-unknown-linux-gnu`, `install_only_stripped` archives. These are native
Linux interpreters, not a Windows DLL or proof of Linux sender compatibility.
The [pinned upstream running guide](https://github.com/astral-sh/python-build-standalone/blob/20261013/docs/running.rst)
describes a minimum glibc version of 2.17 for these targets. The downloaded x64
interpreter and libpython symbol versions also reach at most `GLIBC_2.17`.
ARM64 has release metadata pins only; its archive was not downloaded or run.
Musl-only hosts and different architectures are not covered by these pins.

The x64 archive was independently size/hash checked: 35,099,487 compressed
bytes, 4,526 members, 99,148,363 expanded file bytes. Preserve the original
`python/` distribution, including its license/notice files and bundled standard
library. The inspected install-only archive includes CPython's
`python/lib/python3.13/LICENSE.txt` and pip notices; this observation does not
claim that every native dependency's full build-license metadata is present.
Upstream explains its varied licensing in the
[licensing section](https://github.com/astral-sh/python-build-standalone/blob/20261013/docs/running.rst#licensing).

Extraction must allow safe relative internal links: the real x64 archive has
1,048 symlinks, including `python/bin/python3 -> python3.13`. Rejecting every
symlink makes this legitimate archive unusable. Conversely, never allow
absolute targets, traversal outside a private extraction root or overwrite of
existing user files. Hash verification precedes extraction and execution.
`validate_linux_python(pin)` validates only the fixed metadata contract; it is
not an extractor. The bootstrap owns safe extraction and process lifetime.

## Validation scope

Portable fixture tests exercise exact output bytes, omitted server files,
wrong sources/hashes/architectures, missing notices, traversal, duplicate paths
and links. A separate private run validates the actual official OpenSSH ZIP
without executing its contents. No headset, Steam, SSH server or system
installation is needed by these tests.
