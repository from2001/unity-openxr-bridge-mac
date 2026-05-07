#!/usr/bin/env bash
set -euo pipefail

# Run the end-to-end Unity Play Mode to Quest development workflow.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/run-metalxr-playmode-workflow.sh

Environment:
  ADB                                  Optional adb executable path.
  UNITY_PROJECT_PATH                   Unity project path. Defaults to TestProjects/UnityOpenXRSmoke.
  METALXR_HOST_PORT                    Host stream/control TCP port. Defaults to 47000.
  METALXR_WORKFLOW_BUILD_MISSING       Build missing runtime/host/APK artifacts. Defaults to 1.
  METALXR_WORKFLOW_INSTALL_APK         Install the built Quest APK before launch. Defaults to 1.
  METALXR_WORKFLOW_AUTO_PLAY           Use uloop to enter Play Mode after Unity launches. Defaults to 1.
  METALXR_WORKFLOW_RESTART_UNITY       Quit an existing Unity editor before launch. Defaults to 1.
  METALXR_WORKFLOW_KEEP_RUNNING        Keep Unity and streamer running after smoke checks. Defaults to 1.
  METALXR_WORKFLOW_UNITY_READY_SECONDS Seconds to wait for Unity/uloop readiness. Defaults to 600.
  METALXR_QUEST_ACTIVITY               Android activity class. Defaults to UnityPlayerGameActivity.
  METALXR_QUEST_LAUNCH_CATEGORY        Android launch category. Defaults to Quest VR category.
  METALXR_QUEST_SURFACE_DECODE         Pass the experimental Surface decode opt-in to Quest. Defaults to 0.
  METALXR_QUEST_SURFACE_PRESENT        Pass the experimental Surface external-texture presentation opt-in to Quest. Defaults to 0.
  METALXR_WORKFLOW_EXPORT_WAIT_SECONDS Seconds to wait for Unity frame exports. Defaults to 120.
  METALXR_WORKFLOW_STREAM_WAIT_SECONDS Seconds to wait for streamer/client logs. Defaults to 60.
  METALXR_FRAME_EXPORT_DIR             Shared frame export directory. Defaults to TMPDIR/metalxr_frame_export.
                                      Set to an empty value with METALXR_FRAME_EXPORT_SOCKET for socket-only export.
  METALXR_FRAME_EXPORT_SOCKET          Runtime frame export datagram socket for unity-export source.
  METALXR_FRAME_EXPORT_ACK_SOCKET      Runtime frame slot release ack datagram socket for IOSurface export.
                                      Defaults to METALXR_FRAME_EXPORT_SOCKET.ack when socket export is enabled.
  METALXR_FRAME_EXPORT_MODE            readback or fixture. Defaults to fixture for smoke reliability.
                                      iosurface is experimental and requires
                                      METALXR_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1.
  METALXR_SWAPCHAIN_STORAGE_MODE       shared, managed, or private. Defaults to shared for Unity export.
  METALXR_WORKFLOW_QUALITY             debug, balanced, or native. Defaults to debug for fixture and balanced for readback.
                                      debug: 640x360 at 8 Mbps. balanced: 1344x1408 at 40 Mbps.
                                      native: 1832x1920 at 80 Mbps.
  METALXR_VIEW_WIDTH                   Runtime eye width. Defaults to the selected quality preset.
  METALXR_VIEW_HEIGHT                  Runtime eye height. Defaults to the selected quality preset.
  METALXR_STREAM_BITRATE               H.264 bitrate. Defaults to the selected quality preset.
  METALXR_FRAME_DUMP_DIR               Runtime frame dump directory. Defaults to TMPDIR/metalxr_unity_frames.
  METALXR_RUNTIME_LOG                  Runtime log path. Defaults to TMPDIR/metalxr_unity_runtime.log.
  METALXR_UNITY_LAUNCH_LOG             Unity launch log path. Defaults to TMPDIR/metalxr_unity_launch.log.
  METALXR_STREAMER_LOG                 Host streamer log path. Defaults to TMPDIR/metalxr_host_streamer.log.
  METALXR_SCREENSHOT_PATH              Quest screenshot path. Defaults to TMPDIR/metalxr_quest_stream.png.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_path="${UNITY_PROJECT_PATH:-$repo_root/TestProjects/UnityOpenXRSmoke}"
package_name="${METALXR_QUEST_PACKAGE:-com.metalxr.questclient}"
quest_activity="${METALXR_QUEST_ACTIVITY:-com.unity3d.player.UnityPlayerGameActivity}"
quest_launch_category="${METALXR_QUEST_LAUNCH_CATEGORY:-com.oculus.intent.category.VR}"
quest_surface_decode="${METALXR_QUEST_SURFACE_DECODE:-0}"
quest_surface_present="${METALXR_QUEST_SURFACE_PRESENT:-0}"
host_port="${METALXR_HOST_PORT:-47000}"
build_missing="${METALXR_WORKFLOW_BUILD_MISSING:-1}"
install_apk="${METALXR_WORKFLOW_INSTALL_APK:-1}"
auto_play="${METALXR_WORKFLOW_AUTO_PLAY:-1}"
restart_unity="${METALXR_WORKFLOW_RESTART_UNITY:-1}"
keep_running="${METALXR_WORKFLOW_KEEP_RUNNING:-1}"
unity_ready_seconds="${METALXR_WORKFLOW_UNITY_READY_SECONDS:-600}"
export_wait_seconds="${METALXR_WORKFLOW_EXPORT_WAIT_SECONDS:-120}"
stream_wait_seconds="${METALXR_WORKFLOW_STREAM_WAIT_SECONDS:-60}"
tmp_root="${TMPDIR:-/tmp}"
if [[ "${METALXR_FRAME_EXPORT_DIR+x}" == "x" ]]; then
  frame_export_dir="$METALXR_FRAME_EXPORT_DIR"
else
  frame_export_dir="$tmp_root/metalxr_frame_export"
fi
frame_export_socket="${METALXR_FRAME_EXPORT_SOCKET:-}"
if [[ "${METALXR_FRAME_EXPORT_ACK_SOCKET+x}" == "x" ]]; then
  frame_export_ack_socket="$METALXR_FRAME_EXPORT_ACK_SOCKET"
elif [[ -n "$frame_export_socket" ]]; then
  frame_export_ack_socket="$frame_export_socket.ack"
else
  frame_export_ack_socket=""
fi
frame_export_mode="${METALXR_FRAME_EXPORT_MODE:-fixture}"
swapchain_storage_mode="${METALXR_SWAPCHAIN_STORAGE_MODE:-shared}"
workflow_quality="${METALXR_WORKFLOW_QUALITY:-}"
if [[ -z "$workflow_quality" ]]; then
  if [[ "$frame_export_mode" == "readback" ]]; then
    workflow_quality="balanced"
  else
    workflow_quality="debug"
  fi
fi
case "$workflow_quality" in
  debug)
    preset_view_width=640
    preset_view_height=360
    preset_stream_bitrate=8000000
    ;;
  balanced)
    preset_view_width=1344
    preset_view_height=1408
    preset_stream_bitrate=40000000
    ;;
  native)
    preset_view_width=1832
    preset_view_height=1920
    preset_stream_bitrate=80000000
    ;;
  *)
    echo "METALXR_WORKFLOW_QUALITY must be debug, balanced, or native, got: $workflow_quality" >&2
    exit 1
    ;;
esac
view_width="${METALXR_VIEW_WIDTH:-$preset_view_width}"
view_height="${METALXR_VIEW_HEIGHT:-$preset_view_height}"
stream_bitrate="${METALXR_STREAM_BITRATE:-$preset_stream_bitrate}"
frame_dump_dir="${METALXR_FRAME_DUMP_DIR:-$tmp_root/metalxr_unity_frames}"
runtime_log="${METALXR_RUNTIME_LOG:-$tmp_root/metalxr_unity_runtime.log}"
unity_log="${METALXR_UNITY_LAUNCH_LOG:-$tmp_root/metalxr_unity_launch.log}"
streamer_log="${METALXR_STREAMER_LOG:-$tmp_root/metalxr_host_streamer.log}"
screenshot_path="${METALXR_SCREENSHOT_PATH:-$tmp_root/metalxr_quest_stream.png}"
apk_path="$project_path/Builds/MetalXRQuestClient.apk"
runtime_dylib="$repo_root/Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib"
runtime_manifest="$repo_root/Runtime/MetalXRRuntime/metalxr_runtime.json"
host_streamer="$repo_root/Runtime/MetalXRHost/build/metalxr_host_streamer"
socket_only_export=0
if [[ -z "$frame_export_dir" && -n "$frame_export_socket" ]]; then
  socket_only_export=1
fi

if [[ "$frame_export_mode" == "iosurface" &&
      "${METALXR_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT:-0}" != "1" ]]; then
  echo "METALXR_FRAME_EXPORT_MODE=iosurface is experimental and is disabled by default." >&2
  echo "Set METALXR_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1 only for isolated IOSurface runtime validation." >&2
  exit 1
fi
if [[ -z "$frame_export_dir" && -z "$frame_export_socket" ]]; then
  echo "METALXR_FRAME_EXPORT_DIR or METALXR_FRAME_EXPORT_SOCKET is required." >&2
  exit 1
fi
if [[ "$frame_export_mode" != "iosurface" && -z "$frame_export_dir" ]]; then
  echo "METALXR_FRAME_EXPORT_MODE=$frame_export_mode requires METALXR_FRAME_EXPORT_DIR." >&2
  exit 1
fi

adb_path="${ADB:-}"
if [[ -z "$adb_path" && -x "$repo_root/Libs/adb-lib/adb" ]]; then
  adb_path="$repo_root/Libs/adb-lib/adb"
fi
if [[ -z "$adb_path" ]]; then
  adb_path="$(command -v adb || true)"
fi
if [[ -z "$adb_path" || ! -x "$adb_path" ]]; then
  echo "adb was not found. Set ADB=/path/to/adb." >&2
  exit 1
fi

adb_cmd() {
  if [[ -n "${ANDROID_SERIAL:-}" ]]; then
    "$adb_path" -s "$ANDROID_SERIAL" "$@"
  else
    "$adb_path" "$@"
  fi
}

unity_pid=""
streamer_pid=""
workflow_complete=0

run_cleanup_command_with_timeout() {
  local timeout_seconds="$1"
  shift

  "$@" >/dev/null 2>&1 &
  local cleanup_pid="$!"
  for _ in $(seq 1 "$timeout_seconds"); do
    if ! kill -0 "$cleanup_pid" >/dev/null 2>&1; then
      wait "$cleanup_pid" >/dev/null 2>&1 || true
      return
    fi
    sleep 1
  done

  kill "$cleanup_pid" >/dev/null 2>&1 || true
  wait "$cleanup_pid" >/dev/null 2>&1 || true
}

terminate_pid_with_timeout() {
  local pid="$1"
  local timeout_seconds="$2"

  if [[ -z "$pid" ]] || ! kill -0 "$pid" >/dev/null 2>&1; then
    return
  fi

  kill "$pid" >/dev/null 2>&1 || true
  for _ in $(seq 1 "$timeout_seconds"); do
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      wait "$pid" >/dev/null 2>&1 || true
      return
    fi
    sleep 1
  done

  pkill -TERM -P "$pid" >/dev/null 2>&1 || true
  kill "$pid" >/dev/null 2>&1 || true
  sleep 1
  pkill -KILL -P "$pid" >/dev/null 2>&1 || true
  kill -KILL "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
}

cleanup() {
  if [[ "$keep_running" == "1" && "$workflow_complete" == "1" ]]; then
    return
  fi
  terminate_pid_with_timeout "$streamer_pid" 5
  run_cleanup_command_with_timeout 10 adb_cmd shell am force-stop "$package_name"
  if command -v uloop >/dev/null 2>&1; then
    run_cleanup_command_with_timeout 10 uloop control-play-mode --project-path "$project_path" --action Stop
    run_cleanup_command_with_timeout 10 uloop launch --quit "$project_path"
  fi
  if [[ -n "$unity_pid" ]] && kill -0 "$unity_pid" >/dev/null 2>&1; then
    for _ in $(seq 1 30); do
      if ! kill -0 "$unity_pid" >/dev/null 2>&1; then
        break
      fi
      sleep 1
    done
  fi
  terminate_pid_with_timeout "$unity_pid" 5
}
trap cleanup EXIT

require_ready_device() {
  "$adb_path" start-server >/dev/null
  if [[ -n "${ANDROID_SERIAL:-}" ]]; then
    local device_state
    device_state="$(adb_cmd get-state 2>/dev/null || true)"
    if [[ "$device_state" != "device" ]]; then
      echo "The requested Android device is not authorized or connected: $ANDROID_SERIAL" >&2
      "$adb_path" devices -l >&2 || true
      exit 1
    fi
    return
  fi

  local devices
  devices="$("$adb_path" devices | awk 'NR > 1 && $2 == "device" { print $1 }')"
  if [[ -z "$devices" ]]; then
    echo "No authorized Quest/Android device is connected." >&2
    "$adb_path" devices -l >&2 || true
    exit 1
  fi

  local device_count
  device_count="$(printf '%s\n' "$devices" | sed '/^$/d' | wc -l | tr -d ' ')"
  if [[ "$device_count" != "1" ]]; then
    echo "More than one device is connected. Set ANDROID_SERIAL before running this workflow." >&2
    "$adb_path" devices -l >&2 || true
    exit 1
  fi
}

wait_for_file_pattern() {
  local description="$1"
  local timeout_seconds="$2"
  local pattern_file="$3"
  local grep_pattern="$4"
  local start
  start="$(date +%s)"
  while true; do
    if [[ -f "$pattern_file" ]] && grep -q "$grep_pattern" "$pattern_file"; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if (( now - start >= timeout_seconds )); then
      echo "Timed out waiting for $description." >&2
      return 1
    fi
    sleep 1
  done
}

wait_for_export_pair() {
  local index="$frame_export_dir/frames.jsonl"
  local start
  start="$(date +%s)"
  while true; do
    if [[ -f "$index" ]] && grep -q '"eye":0' "$index" && grep -q '"eye":1' "$index"; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if (( now - start >= export_wait_seconds )); then
      echo "Timed out waiting for Unity frame exports in $frame_export_dir." >&2
      echo "Check Unity Play Mode and runtime log: $runtime_log" >&2
      return 1
    fi
    sleep 1
  done
}

build_missing_artifacts() {
  if [[ "$build_missing" != "1" ]]; then
    return
  fi
  if [[ ! -f "$runtime_dylib" || ! -f "$runtime_manifest" ]]; then
    "$repo_root/Scripts/build-metalxr-runtime.sh"
  fi
  if [[ ! -x "$host_streamer" ]]; then
    "$repo_root/Scripts/build-metalxr-host.sh"
  fi
  if [[ ! -f "$apk_path" ]]; then
    "$repo_root/Scripts/build-quest-client-apk.sh" "$apk_path"
  fi
}

prepare_quest_client() {
  if [[ "$install_apk" == "1" ]]; then
    "$repo_root/Scripts/install-quest-apk.sh" "$apk_path"
  fi
  adb_cmd reverse --remove "tcp:$host_port" >/dev/null 2>&1 || true
  adb_cmd reverse "tcp:$host_port" "tcp:$host_port"
}

start_quest_client_activity() {
  adb_cmd shell am force-stop "$package_name" >/dev/null 2>&1 || true
  adb_cmd logcat -c >/dev/null 2>&1 || true

  if [[ "$quest_surface_decode" == "1" && "$quest_surface_present" == "1" ]]; then
    adb_cmd shell am start -W \
      -a android.intent.action.MAIN \
      -c "$quest_launch_category" \
      -n "$package_name/$quest_activity" \
      --ez metalxr_surface_decode true \
      --ez metalxr_surface_present true >/dev/null
    return
  fi

  if [[ "$quest_surface_decode" == "1" ]]; then
    adb_cmd shell am start -W \
      -a android.intent.action.MAIN \
      -c "$quest_launch_category" \
      -n "$package_name/$quest_activity" \
      --ez metalxr_surface_decode true >/dev/null
    return
  fi

  if [[ "$quest_surface_present" == "1" ]]; then
    adb_cmd shell am start -W \
      -a android.intent.action.MAIN \
      -c "$quest_launch_category" \
      -n "$package_name/$quest_activity" \
      --ez metalxr_surface_present true >/dev/null
    return
  fi

  adb_cmd shell am start -W \
    -a android.intent.action.MAIN \
    -c "$quest_launch_category" \
    -n "$package_name/$quest_activity" >/dev/null
}

launch_quest_client() {
  prepare_quest_client
  start_quest_client_activity
}

launch_unity_and_enter_play_mode() {
  mkdir -p "$(dirname "$unity_log")" "$frame_dump_dir"
  if [[ -n "$frame_export_dir" ]]; then
    mkdir -p "$frame_export_dir"
  fi
  rm -f "$unity_log" "$runtime_log"
  if [[ -n "$frame_export_dir" ]]; then
    rm -rf "$frame_export_dir"
    mkdir -p "$frame_export_dir"
  fi
  rm -rf "$frame_dump_dir"
  mkdir -p "$frame_dump_dir"

  cat >"$unity_log" <<EOF
Repository: $repo_root
Unity project: $project_path
OpenXR runtime: $runtime_manifest
MetalXR runtime log: $runtime_log
MetalXR frame dumps: $frame_dump_dir
MetalXR frame exports: ${frame_export_dir:-disabled}
MetalXR frame export socket: ${frame_export_socket:-disabled}
MetalXR frame export mode: $frame_export_mode
MetalXR swapchain storage mode: $swapchain_storage_mode
MetalXR workflow quality: $workflow_quality
MetalXR view size: ${view_width}x${view_height}
MetalXR stream bitrate: $stream_bitrate
MetalXR Unity ready timeout: ${unity_ready_seconds}s
MetalXR tracking state: ${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}
MetalXR haptic commands: ${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}
MetalXR timing state: ${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}
MetalXR shared state: ${METALXR_SHARED_STATE_NAME:-/metalxr_runtime_state}
EOF

  if [[ "$auto_play" == "1" ]]; then
    if ! command -v uloop >/dev/null 2>&1; then
      echo "uloop is required when METALXR_WORKFLOW_AUTO_PLAY=1." >&2
      echo "Set METALXR_WORKFLOW_AUTO_PLAY=0 to launch Unity and enter Play Mode manually." >&2
      return 1
    fi

    if [[ "$restart_unity" == "1" ]]; then
      uloop launch --quit "$project_path" >>"$unity_log" 2>&1 || true
    fi

    METALXR_RUNTIME_JSON="$runtime_manifest" \
    METALXR_RUNTIME_LOG="$runtime_log" \
    METALXR_FRAME_DUMP_DIR="$frame_dump_dir" \
    METALXR_FRAME_EXPORT_DIR="$frame_export_dir" \
    METALXR_FRAME_EXPORT_SOCKET="$frame_export_socket" \
    METALXR_FRAME_EXPORT_ACK_SOCKET="$frame_export_ack_socket" \
    METALXR_FRAME_EXPORT_MODE="$frame_export_mode" \
    METALXR_SWAPCHAIN_STORAGE_MODE="$swapchain_storage_mode" \
    METALXR_VIEW_WIDTH="$view_width" \
    METALXR_VIEW_HEIGHT="$view_height" \
    METALXR_START_ULOOP_SERVER=1 \
    METALXR_TRACKING_STATE_PATH="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}" \
    METALXR_HAPTIC_COMMAND_PATH="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}" \
    METALXR_TIMING_STATE_PATH="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}" \
    METALXR_SHARED_STATE_NAME="${METALXR_SHARED_STATE_NAME:-/metalxr_runtime_state}" \
    METALXR_DISABLE_SHARED_STATE="${METALXR_DISABLE_SHARED_STATE:-0}" \
    "$repo_root/Scripts/launch-unity-openxr.sh" "$project_path" >>"$unity_log" 2>&1 &
    unity_pid="$!"

    local ready=0
    local unity_ready_polls=$(((unity_ready_seconds + 1) / 2))
    if (( unity_ready_polls < 1 )); then
      unity_ready_polls=1
    fi
    for _ in $(seq 1 "$unity_ready_polls"); do
      if [[ -n "$unity_pid" ]] && ! kill -0 "$unity_pid" >/dev/null 2>&1; then
        echo "Unity exited before uloop became ready. Check $unity_log." >&2
        {
          echo
          echo "Unity process $unity_pid exited before uloop became ready."
          echo "Recent Unity Editor.log:"
          tail -n 120 "$HOME/Library/Logs/Unity/Editor.log" 2>/dev/null || true
        } >>"$unity_log"
        return 1
      fi
      if uloop compile --project-path "$project_path" --force-recompile false --wait-for-domain-reload true >>"$unity_log" 2>&1; then
        ready=1
        break
      fi
      sleep 2
    done
    if [[ "$ready" != "1" ]]; then
      echo "Unity did not become ready for uloop. Check $unity_log." >&2
      return 1
    fi
    uloop compile --project-path "$project_path" --force-recompile false --wait-for-domain-reload true >>"$unity_log" 2>&1

    if [[ "$socket_only_export" == "1" && -z "$streamer_pid" ]]; then
      start_unity_export_streamer
      start_quest_client_activity
    fi

    local play_ready=0
    local play_output
    for _ in $(seq 1 30); do
      if [[ -n "$unity_pid" ]] && ! kill -0 "$unity_pid" >/dev/null 2>&1; then
        echo "Unity exited before entering Play Mode. Check $unity_log." >&2
        {
          echo
          echo "Unity process $unity_pid exited before entering Play Mode."
          echo "Recent Unity Editor.log:"
          tail -n 120 "$HOME/Library/Logs/Unity/Editor.log" 2>/dev/null || true
        } >>"$unity_log"
        return 1
      fi
      play_output="$(uloop control-play-mode --project-path "$project_path" --action Play 2>&1)" || {
        printf '%s\n' "$play_output" >>"$unity_log"
        sleep 1
        continue
      }
      printf '%s\n' "$play_output" >>"$unity_log"
      if printf '%s\n' "$play_output" | grep -q '"IsPlaying": true'; then
        play_ready=1
        break
      fi
      sleep 1
    done
    if [[ "$play_ready" != "1" ]]; then
      echo "Unity did not enter Play Mode. Check $unity_log." >&2
      return 1
    fi
    return
  fi

  METALXR_RUNTIME_JSON="$runtime_manifest" \
  METALXR_RUNTIME_LOG="$runtime_log" \
  METALXR_FRAME_DUMP_DIR="$frame_dump_dir" \
  METALXR_FRAME_EXPORT_DIR="$frame_export_dir" \
  METALXR_FRAME_EXPORT_SOCKET="$frame_export_socket" \
  METALXR_FRAME_EXPORT_ACK_SOCKET="$frame_export_ack_socket" \
  METALXR_FRAME_EXPORT_MODE="$frame_export_mode" \
  METALXR_SWAPCHAIN_STORAGE_MODE="$swapchain_storage_mode" \
  METALXR_VIEW_WIDTH="$view_width" \
  METALXR_VIEW_HEIGHT="$view_height" \
  METALXR_TRACKING_STATE_PATH="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}" \
  METALXR_HAPTIC_COMMAND_PATH="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}" \
  METALXR_TIMING_STATE_PATH="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}" \
  METALXR_SHARED_STATE_NAME="${METALXR_SHARED_STATE_NAME:-/metalxr_runtime_state}" \
  METALXR_DISABLE_SHARED_STATE="${METALXR_DISABLE_SHARED_STATE:-0}" \
  "$repo_root/Scripts/launch-unity-openxr.sh" "$project_path" >"$unity_log" 2>&1 &
  unity_pid="$!"

  echo "Unity launched. Enter Play Mode manually, then wait for frame exports in $frame_export_dir."
}

start_unity_export_streamer() {
  mkdir -p "$(dirname "$streamer_log")"
  rm -f "$streamer_log"
  METALXR_TRANSPORT=usb \
  METALXR_HOST_PORT="$host_port" \
  METALXR_FRAME_SOURCE=unity-export \
  METALXR_FRAME_EXPORT_DIR="$frame_export_dir" \
  METALXR_FRAME_EXPORT_SOCKET="$frame_export_socket" \
  METALXR_FRAME_EXPORT_ACK_SOCKET="$frame_export_ack_socket" \
  METALXR_FRAME_EXPORT_WAIT_MS="$((export_wait_seconds * 1000))" \
  METALXR_STREAM_BITRATE="$stream_bitrate" \
  METALXR_TRACKING_STATE_PATH="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}" \
  METALXR_HAPTIC_COMMAND_PATH="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}" \
  METALXR_TIMING_STATE_PATH="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}" \
  METALXR_SHARED_STATE_NAME="${METALXR_SHARED_STATE_NAME:-/metalxr_runtime_state}" \
  METALXR_DISABLE_SHARED_STATE="${METALXR_DISABLE_SHARED_STATE:-0}" \
  "$repo_root/Scripts/run-metalxr-frame-stream.sh" >"$streamer_log" 2>&1 &
  streamer_pid="$!"
}

wait_for_quest_display_logs() {
  local start
  start="$(date +%s)"
  while true; do
    if adb_cmd logcat -d Unity:D '*:S' | grep -q 'MetalXRQuestClient displayed VIDEO_FRAME'; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if (( now - start >= stream_wait_seconds )); then
      echo "Timed out waiting for Quest display logs." >&2
      return 1
    fi
    sleep 1
  done
}

require_ready_device
build_missing_artifacts
prepare_quest_client
launch_unity_and_enter_play_mode
if [[ "$socket_only_export" == "1" && -z "$streamer_pid" ]]; then
  start_unity_export_streamer
  start_quest_client_activity
fi
if [[ "$socket_only_export" != "1" ]]; then
  wait_for_export_pair
  start_unity_export_streamer
  start_quest_client_activity
fi
wait_for_file_pattern "Quest streamer connection" "$stream_wait_seconds" "$streamer_log" 'Quest stream client connected'
wait_for_file_pattern "streamed VIDEO_FRAME packets" "$stream_wait_seconds" "$streamer_log" '"event":"streamed"'
wait_for_quest_display_logs

METALXR_SCREENSHOT_PATH="$screenshot_path" "$repo_root/Scripts/probe-quest-client-screenshot.sh"

echo "MetalXR Play Mode workflow smoke passed."
echo "Unity log: $unity_log"
echo "Runtime log: $runtime_log"
echo "Frame exports: ${frame_export_dir:-disabled}"
echo "Frame export socket: ${frame_export_socket:-disabled}"
echo "Streamer log: $streamer_log"
echo "Quest screenshot: $screenshot_path"
workflow_complete=1

if [[ "$keep_running" == "1" ]]; then
  echo "Unity and host streamer are left running. Press Ctrl-C to stop the foreground streamer wait."
  wait "$streamer_pid"
fi
