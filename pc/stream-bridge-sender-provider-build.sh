#!/usr/bin/env bash
# Offline development build only. No SDK downloads, registration or installation.
set -euo pipefail
if [ "$#" -ne 3 ]; then
  echo 'Usage: stream-bridge-sender-provider-build.sh SDK_DIRECTORY PROTOCOL_SOURCE_ROOT OUTPUT_DIRECTORY' >&2
  exit 2
fi
sender_sdk=$(realpath -e -- "$1")
sender_protocol=$(realpath -e -- "$2")
sender_output=$(realpath -m -- "$3")
sender_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 - "$sender_sdk" <<'PY'
from pathlib import Path
import hashlib,sys
root=Path(sys.argv[1])
for name,expected in {
 'nvEncodeAPI.h':'be0c29a41ae3260fd016a9001fd959e747bac93ad21411759736abba4ffdea86',
 'openvr_driver.h':'1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab',
}.items():
 if hashlib.sha256((root/name).read_bytes()).hexdigest()!=expected:
  raise SystemExit('Explicit SDK header does not match the reviewed pin: '+name)
PY
mkdir -p -- "$sender_output"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I"$sender_repo" -I"$sender_protocol" -isystem "$sender_sdk" \
  "$sender_repo/pc/stream-bridge-sender-provider.cpp" \
  "$sender_repo/pc/stream-bridge-sender-provider-core.cpp" \
  "$sender_repo/pc/stream-bridge-sender-provider-directmode.cpp" \
  "$sender_repo/pc/stream-bridge-sender-provider-runtime.cpp" \
  "$sender_repo/pc/stream-bridge-sender.cpp" \
  "$sender_protocol/bridge/stream_bridge_protocol.cpp" \
  "$sender_protocol/bridge/stream_bridge_auth.cpp" \
  "$sender_protocol/bridge/stream_bridge_frame.cpp" \
  "$sender_protocol/bridge/stream_bridge_credentials.cpp" \
  "$sender_protocol/bridge/stream_bridge_control.cpp" \
  "$sender_protocol/receiver/stream_bridge_receiver.cpp" \
  "$sender_protocol/bridge/hevc_bridge_sei.cpp" \
  -shared -s -Wl,--no-insert-timestamp -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic \
  -lbcrypt -lws2_32 -ladvapi32 -lole32 -lshell32 -luuid -o "$sender_output/driver_steam_link_native_passthrough.dll"
cp -- "$sender_repo/pc/stream-bridge-sender-provider.vrdrivermanifest" "$sender_output/driver.vrdrivermanifest"
sha256sum -- "$sender_output/driver_steam_link_native_passthrough.dll"
