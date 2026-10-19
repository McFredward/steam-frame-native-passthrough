#!/usr/bin/env bash
# No download, elevation, service restart or background installation.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
command -v python3 >/dev/null 2>&1 || { echo 'Python 3.9 or newer is required. Install it with your normal system tools, then run this file again.' >&2; exit 2; }
command -v ssh >/dev/null 2>&1 || { echo 'OpenSSH client is required. Install it with your normal system tools, then run this file again.' >&2; exit 2; }
engine="$root/steam_link_setup.py"
[[ -f "$engine" ]] || engine="$root/setup/steam_link_setup.py"
exec python3 -B "$engine" "$@"
