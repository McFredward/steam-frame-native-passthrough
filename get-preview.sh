#!/usr/bin/env bash
# Compatibility alias for the same paired-only setup; device changes require a selected setup action.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/dcb6a273ccb25e159e1d4fd2410c28fea64f70db/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '72be7a71ad67314feaa1e654bf0eada98f2d9da025833ffc145c7651a78bc948' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --bridge-preview
