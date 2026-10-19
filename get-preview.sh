#!/usr/bin/env bash
# Compatibility alias for the same paired-only setup; device changes require a selected setup action.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/2b0bcb97c56cf391b3e00c6cec0c55439495d686/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '3407ef2d89095e1ed72e656f3ffa9ea03e420038328f49662af8eb6ee2360010' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --bridge-preview
