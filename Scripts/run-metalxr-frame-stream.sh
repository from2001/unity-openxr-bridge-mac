#!/usr/bin/env bash
set -euo pipefail

# Run the macOS host frame streamer for a connected Quest client.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/run-metalxr-frame-stream.sh

Environment:
  ADB                         Optional adb executable path. Defaults to Libs/adb-lib/adb, then PATH.
  METALXR_TRANSPORT           usb or wifi. Defaults to usb.
  METALXR_HOST_BIND           Host address to bind. Defaults to 127.0.0.1 for usb, 0.0.0.0 for wifi.
  METALXR_HOST_PORT           Stream/control TCP port. Defaults to 47000.
  METALXR_STREAM_WIDTH        Encoded eye width. Defaults to 640.
  METALXR_STREAM_HEIGHT       Encoded eye height. Defaults to 360.
  METALXR_STREAM_FPS          Stream frame rate. Defaults to 60.
  METALXR_STREAM_BITRATE      H.264 bitrate in bits per second. Defaults to 8000000.
  METALXR_STREAM_FRAMES       Frame count. Defaults to 0, which streams until disconnect.
  METALXR_TRACKING_STATE_PATH Host tracking state output. Defaults to /tmp/metalxr_tracking_state.txt.
  METALXR_HAPTIC_COMMAND_PATH Runtime haptic command input. Defaults to /tmp/metalxr_haptic_command.txt.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
streamer="$repo_root/Runtime/MetalXRHost/build/metalxr_host_streamer"
transport="${METALXR_TRANSPORT:-usb}"
port="${METALXR_HOST_PORT:-47000}"
width="${METALXR_STREAM_WIDTH:-640}"
height="${METALXR_STREAM_HEIGHT:-360}"
fps="${METALXR_STREAM_FPS:-60}"
bitrate="${METALXR_STREAM_BITRATE:-8000000}"
frames="${METALXR_STREAM_FRAMES:-0}"
tracking_state_path="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}"
haptic_command_path="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}"

if [[ ! -x "$streamer" ]]; then
  "$repo_root/Scripts/build-metalxr-host.sh"
fi

bind_host="${METALXR_HOST_BIND:-}"
if [[ -z "$bind_host" ]]; then
  if [[ "$transport" == "usb" ]]; then
    bind_host="127.0.0.1"
  else
    bind_host="0.0.0.0"
  fi
fi

if [[ "$transport" == "usb" ]]; then
  adb_path="${ADB:-}"
  if [[ -z "$adb_path" && -x "$repo_root/Libs/adb-lib/adb" ]]; then
    adb_path="$repo_root/Libs/adb-lib/adb"
  fi
  if [[ -z "$adb_path" ]]; then
    adb_path="$(command -v adb || true)"
  fi
  if [[ -z "$adb_path" || ! -x "$adb_path" ]]; then
    echo "adb was not found. Set ADB=/path/to/adb or use METALXR_TRANSPORT=wifi." >&2
    exit 1
  fi

  echo "Forwarding Quest tcp:$port to host tcp:$port with adb reverse"
  "$adb_path" reverse "tcp:$port" "tcp:$port"
elif [[ "$transport" != "wifi" ]]; then
  echo "METALXR_TRANSPORT must be usb or wifi, got: $transport" >&2
  exit 1
fi

exec "$streamer" \
  --bind-host "$bind_host" \
  --port "$port" \
  --frames "$frames" \
  --fps "$fps" \
  --width "$width" \
  --height "$height" \
  --bitrate "$bitrate" \
  --tracking-state-path "$tracking_state_path" \
  --haptic-command-path "$haptic_command_path"
