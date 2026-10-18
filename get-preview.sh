#!/usr/bin/env bash
# Explicit paired-bridge preview; device changes require a selected setup action.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/e5cec7328e09a70bccd61393a52158d0ca2f838a/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '33ce7f247664b2159973aa71d6849e1c06a12f8a79c61259dc7c4511c7dec411' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --bridge-preview
