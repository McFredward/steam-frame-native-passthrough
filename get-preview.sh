#!/usr/bin/env bash
# Explicit paired-bridge preview; device changes require a selected setup action.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/31801d13a350be6e67a845357bb8136b9cf7e627/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' 'a8b46e986daee6a94fef3f8d9ad78e1f8a13aff8d44d4096c1fe2657141b963f' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --bridge-preview
