#!/usr/bin/env bash
# Read-only preview; use the reviewed bootstrap with explicit --inspect.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/2c2fdeeef5ce623c55b2c8489872531f8226a238/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' 'c40a871131332d992b260aca89d6cf8111a14a52523b1959d45e27d8d07f6143' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --inspect
