#!/usr/bin/env bash
# Compatibility alias for the same paired-only setup; device changes require a selected setup action.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/f6a29075a7426323a02372a951af82145b8f8fad/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '60c474b235557976b3fe58380b1a728271f62821fad4d8fbb5c6cb5d58922462' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --bridge-preview
