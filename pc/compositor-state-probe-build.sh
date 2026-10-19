#!/usr/bin/env bash
set -euo pipefail
# Builds an explicit diagnostic; never launches Windows/OpenVR or downloads SDKs.
if [[ $# != 2 ]]; then
    echo "Usage: $0 <directory-containing-reviewed-openvr_capi.h> <output-directory>" >&2
    exit 2
fi
probe_sdk=$(realpath -- "$1")
probe_output=$(realpath -m -- "$2")
probe_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_header="$probe_sdk/openvr_capi.h"
probe_expected=3d32d8eb5bcbe2e250610123aee8142c68f0e7d4da823f87513b6a2a333078d7
[[ -f "$probe_header" ]] || { echo 'Missing reviewed public OpenVR C header.' >&2; exit 2; }
[[ $(sha256sum -- "$probe_header" | cut -d ' ' -f 1) == "$probe_expected" ]] || {
    echo 'Unreviewed OpenVR C header; refusing ABI guesses.' >&2; exit 2;
}
mkdir -p -- "$probe_output"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$probe_source/compositor-state-probe-test.cpp" -o "$probe_output/compositor-state-probe-test"
"$probe_output/compositor-state-probe-test"
"$probe_output/compositor-state-probe-test" --transcript > "$probe_output/transcript.jsonl"
"$probe_output/compositor-state-probe-test" --refused-transcript > "$probe_output/refused-transcript.jsonl"
"$probe_output/compositor-state-probe-test" --budget-transcript > "$probe_output/budget-transcript.jsonl"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -isystem "$probe_sdk" -municode -static -s -Wl,--no-insert-timestamp \
    "$probe_source/compositor-state-probe.cpp" -o "$probe_output/compositor-state-probe.exe"
sha256sum -- "$probe_output/compositor-state-probe.exe"
