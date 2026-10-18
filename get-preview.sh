#!/usr/bin/env bash
# Guided check/removal; the reviewed bootstrap cannot install an unfinished bridge.
set -euo pipefail
for tool in curl sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Required tool missing: $tool" >&2; exit 2; }
done
sfnp_preview_stage=$(mktemp -d)
trap 'rm -rf -- "$sfnp_preview_stage"' EXIT
curl --fail --silent --show-error --proto '=https' --max-time 30 --max-filesize 65536 \
    'https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/9c37263868e45495adf14589db8ccaeef7c36e95/get.sh' \
    --output "$sfnp_preview_stage/get.sh"
printf '%s  %s\n' '248f0b07e913925f17ed5d2d870488cb97019a2780ad945b0b12bdd8b465f7f5' "$sfnp_preview_stage/get.sh" | sha256sum --check --status
bash "$sfnp_preview_stage/get.sh" --manage
