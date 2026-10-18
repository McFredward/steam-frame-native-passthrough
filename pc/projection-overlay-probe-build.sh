#!/usr/bin/env bash
set -euo pipefail
# Development-only; no SDK/Valve binary download or runtime activation.
if [[ $# != 2 ]]; then
    echo "Usage: $0 <directory-containing-public-openvr_capi.h> <output-directory>" >&2
    exit 2
fi
probe_sdk=$(realpath -- "$1")
probe_output=$(realpath -m -- "$2")
probe_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_header="$probe_sdk/openvr_capi.h"
probe_expected=3d32d8eb5bcbe2e250610123aee8142c68f0e7d4da823f87513b6a2a333078d7
[[ -f "$probe_header" ]] || { echo 'Missing public OpenVR C header.' >&2; exit 2; }
[[ $(sha256sum -- "$probe_header" | cut -d ' ' -f 1) == "$probe_expected" ]] || {
    echo 'Unreviewed OpenVR C header; do not guess function-table slots.' >&2; exit 2;
}
mkdir -p -- "$probe_output"
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
    "$probe_source/projection-overlay-probe-test.cpp" -o "$probe_output/projection-overlay-probe-test"
"$probe_output/projection-overlay-probe-test"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-cast-function-type \
    -isystem "$probe_sdk" -municode -static-libgcc -static-libstdc++ \
    "$probe_source/projection-overlay-probe.cpp" -o "$probe_output/projection-overlay-probe.exe" \
    -ld3d11 -ldxgi -lole32 -luuid
sha256sum -- "$probe_output/projection-overlay-probe.exe"
