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
  METALXR_ADB_REVERSE_REFRESH_SECONDS
                              USB adb reverse refresh interval. Defaults to 5, set 0 to disable.
  METALXR_STREAM_WIDTH        Encoded eye width. Defaults to 640.
  METALXR_STREAM_HEIGHT       Encoded eye height. Defaults to 360.
  METALXR_STREAM_FPS          Stream frame rate. Defaults to 60.
  METALXR_STREAM_BITRATE      H.264 bitrate in bits per second. Defaults to 8000000.
  METALXR_STREAM_FRAMES       Frame count. Defaults to 0, which streams until disconnect.
  METALXR_STREAM_QUEUE_DEPTH  Max pending encoder frames per eye. Defaults to 3.
  METALXR_STREAM_RECONNECT_ATTEMPTS
                              Reconnect attempts after early finite-stream disconnects. Defaults to 0.
  METALXR_FRAME_SOURCE        synthetic or unity-export. Defaults to synthetic.
  METALXR_FRAME_EXPORT_DIR    Runtime frame export directory for unity-export source.
  METALXR_PREDICTION_OFFSET_MS
                              Signed prediction offset in milliseconds. Defaults to 0.
  METALXR_CLOCK_SYNC_INTERVAL_MS
                              Clock sync probe interval. Defaults to 500.
  METALXR_TRACKING_STATE_PATH Host tracking state output. Defaults to /tmp/metalxr_tracking_state.txt.
  METALXR_HAPTIC_COMMAND_PATH Runtime haptic command input. Defaults to /tmp/metalxr_haptic_command.txt.
  METALXR_TIMING_STATE_PATH   Host timing state output. Defaults to /tmp/metalxr_timing_state.txt.
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
adb_reverse_refresh_seconds="${METALXR_ADB_REVERSE_REFRESH_SECONDS:-5}"
width="${METALXR_STREAM_WIDTH:-640}"
height="${METALXR_STREAM_HEIGHT:-360}"
fps="${METALXR_STREAM_FPS:-60}"
bitrate="${METALXR_STREAM_BITRATE:-8000000}"
frames="${METALXR_STREAM_FRAMES:-0}"
queue_depth="${METALXR_STREAM_QUEUE_DEPTH:-3}"
reconnect_attempts="${METALXR_STREAM_RECONNECT_ATTEMPTS:-0}"
frame_source="${METALXR_FRAME_SOURCE:-synthetic}"
frame_export_dir="${METALXR_FRAME_EXPORT_DIR:-}"
prediction_offset_ms="${METALXR_PREDICTION_OFFSET_MS:-0}"
clock_sync_interval_ms="${METALXR_CLOCK_SYNC_INTERVAL_MS:-500}"
tracking_state_path="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}"
haptic_command_path="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}"
timing_state_path="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}"

if [[ ! -x "$streamer" ]]; then
  "$repo_root/Scripts/build-metalxr-host.sh"
fi

adb_reverse_keeper_pid=""

cleanup() {
  if [[ -n "$adb_reverse_keeper_pid" ]] && kill -0 "$adb_reverse_keeper_pid" >/dev/null 2>&1; then
    kill "$adb_reverse_keeper_pid" >/dev/null 2>&1 || true
    wait "$adb_reverse_keeper_pid" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

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
  if [[ "$adb_reverse_refresh_seconds" != "0" ]]; then
    (
      while true; do
        sleep "$adb_reverse_refresh_seconds"
        "$adb_path" reverse "tcp:$port" "tcp:$port" >/dev/null 2>&1 || true
      done
    ) &
    adb_reverse_keeper_pid="$!"
  fi
elif [[ "$transport" != "wifi" ]]; then
  echo "METALXR_TRANSPORT must be usb or wifi, got: $transport" >&2
  exit 1
fi

if [[ "$frame_source" != "synthetic" && "$frame_source" != "unity-export" ]]; then
  echo "METALXR_FRAME_SOURCE must be synthetic or unity-export, got: $frame_source" >&2
  exit 1
fi

if [[ "$frame_source" == "unity-export" && -z "$frame_export_dir" ]]; then
  echo "METALXR_FRAME_EXPORT_DIR is required when METALXR_FRAME_SOURCE=unity-export." >&2
  exit 1
fi

streamer_args=(
  --bind-host "$bind_host"
  --port "$port"
  --frames "$frames"
  --fps "$fps"
  --width "$width"
  --height "$height"
  --bitrate "$bitrate"
  --queue-depth "$queue_depth"
  --reconnect-attempts "$reconnect_attempts"
  --frame-source "$frame_source"
  --prediction-offset-ms "$prediction_offset_ms"
  --clock-sync-interval-ms "$clock_sync_interval_ms"
  --tracking-state-path "$tracking_state_path"
  --haptic-command-path "$haptic_command_path"
  --timing-state-path "$timing_state_path"
)

if [[ -n "$frame_export_dir" ]]; then
  streamer_args+=(--frame-export-dir "$frame_export_dir")
fi

"$streamer" \
  "${streamer_args[@]}"
