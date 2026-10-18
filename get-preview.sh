#!/usr/bin/env bash
# Read-only preview; use the reviewed bootstrap with explicit --inspect.
set -euo pipefail
for tool in curl sha256sum python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/b171247a19b39236f764a5ee5525f0ad4cf4f110/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '04eb9b58ee92e8791adc1f57569b3e9fad7e663164f023bb040a4e04dfd78c88' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --inspect
