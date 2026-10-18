#!/usr/bin/env bash
# Explicit paired-bridge preview; device changes require a selected setup action.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/87aced6d3f1daaca8595287201f1c78f9932f5de/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '8fd0c2a5ab2427e2177fbe244f32d06bbdca1e47b4ffff16d1709a8784556187' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --bridge-preview
