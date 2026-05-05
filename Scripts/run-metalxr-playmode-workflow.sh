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
  METALXR_WORKFLOW_EXPORT_WAIT_SECONDS Seconds to wait for Unity frame exports. Defaults to 120.
  METALXR_WORKFLOW_STREAM_WAIT_SECONDS Seconds to wait for streamer/client logs. Defaults to 60.
  METALXR_FRAME_EXPORT_DIR             Shared frame export directory. Defaults to TMPDIR/metalxr_frame_export.
  METALXR_FRAME_EXPORT_MODE            readback or fixture. Defaults to fixture for smoke reliability.
  METALXR_SWAPCHAIN_STORAGE_MODE       shared, managed, or private. Defaults to shared for Unity export.
  METALXR_VIEW_WIDTH                   Runtime eye width. Defaults to 640 for Quest debug streaming.
  METALXR_VIEW_HEIGHT                  Runtime eye height. Defaults to 360 for Quest debug streaming.
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
host_port="${METALXR_HOST_PORT:-47000}"
build_missing="${METALXR_WORKFLOW_BUILD_MISSING:-1}"
install_apk="${METALXR_WORKFLOW_INSTALL_APK:-1}"
auto_play="${METALXR_WORKFLOW_AUTO_PLAY:-1}"
restart_unity="${METALXR_WORKFLOW_RESTART_UNITY:-1}"
keep_running="${METALXR_WORKFLOW_KEEP_RUNNING:-1}"
export_wait_seconds="${METALXR_WORKFLOW_EXPORT_WAIT_SECONDS:-120}"
stream_wait_seconds="${METALXR_WORKFLOW_STREAM_WAIT_SECONDS:-60}"
tmp_root="${TMPDIR:-/tmp}"
frame_export_dir="${METALXR_FRAME_EXPORT_DIR:-$tmp_root/metalxr_frame_export}"
frame_export_mode="${METALXR_FRAME_EXPORT_MODE:-fixture}"
swapchain_storage_mode="${METALXR_SWAPCHAIN_STORAGE_MODE:-shared}"
view_width="${METALXR_VIEW_WIDTH:-640}"
view_height="${METALXR_VIEW_HEIGHT:-360}"
frame_dump_dir="${METALXR_FRAME_DUMP_DIR:-$tmp_root/metalxr_unity_frames}"
runtime_log="${METALXR_RUNTIME_LOG:-$tmp_root/metalxr_unity_runtime.log}"
unity_log="${METALXR_UNITY_LAUNCH_LOG:-$tmp_root/metalxr_unity_launch.log}"
streamer_log="${METALXR_STREAMER_LOG:-$tmp_root/metalxr_host_streamer.log}"
screenshot_path="${METALXR_SCREENSHOT_PATH:-$tmp_root/metalxr_quest_stream.png}"
apk_path="$project_path/Builds/MetalXRQuestClient.apk"
runtime_dylib="$repo_root/Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib"
runtime_manifest="$repo_root/Runtime/MetalXRRuntime/metalxr_runtime.json"
host_streamer="$repo_root/Runtime/MetalXRHost/build/metalxr_host_streamer"

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
  local devices
  devices="$("$adb_path" devices | awk 'NR > 1 && $2 == "device" { print $1 }')"
  if [[ -z "$devices" ]]; then
    echo "No authorized Quest/Android device is connected." >&2
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

launch_quest_client() {
  if [[ "$install_apk" == "1" ]]; then
    "$repo_root/Scripts/install-quest-apk.sh" "$apk_path"
  fi
  "$adb_path" reverse --remove "tcp:$host_port" >/dev/null 2>&1 || true
  "$adb_path" reverse "tcp:$host_port" "tcp:$host_port"
  "$adb_path" shell am force-stop "$package_name" >/dev/null 2>&1 || true
  "$adb_path" logcat -c >/dev/null 2>&1 || true
  "$adb_path" shell monkey -p "$package_name" -c com.oculus.intent.category.VR 1 >/dev/null
}

launch_unity_and_enter_play_mode() {
  mkdir -p "$(dirname "$unity_log")" "$frame_export_dir" "$frame_dump_dir"
  rm -f "$unity_log" "$runtime_log"
  rm -rf "$frame_export_dir" "$frame_dump_dir"
  mkdir -p "$frame_export_dir" "$frame_dump_dir"

  cat >"$unity_log" <<EOF
Repository: $repo_root
Unity project: $project_path
OpenXR runtime: $runtime_manifest
MetalXR runtime log: $runtime_log
MetalXR frame dumps: $frame_dump_dir
MetalXR frame exports: $frame_export_dir
MetalXR frame export mode: $frame_export_mode
MetalXR swapchain storage mode: $swapchain_storage_mode
MetalXR view size: ${view_width}x${view_height}
MetalXR tracking state: ${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}
MetalXR haptic commands: ${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}
MetalXR timing state: ${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}
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
    METALXR_FRAME_EXPORT_MODE="$frame_export_mode" \
    METALXR_SWAPCHAIN_STORAGE_MODE="$swapchain_storage_mode" \
    METALXR_VIEW_WIDTH="$view_width" \
    METALXR_VIEW_HEIGHT="$view_height" \
    METALXR_START_ULOOP_SERVER=1 \
    METALXR_TRACKING_STATE_PATH="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}" \
    METALXR_HAPTIC_COMMAND_PATH="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}" \
    METALXR_TIMING_STATE_PATH="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}" \
    "$repo_root/Scripts/launch-unity-openxr.sh" "$project_path" >>"$unity_log" 2>&1 &
    unity_pid="$!"

    local ready=0
    for _ in $(seq 1 90); do
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

    local play_ready=0
    local play_output
    for _ in $(seq 1 30); do
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
  METALXR_FRAME_EXPORT_MODE="$frame_export_mode" \
  METALXR_SWAPCHAIN_STORAGE_MODE="$swapchain_storage_mode" \
  METALXR_VIEW_WIDTH="$view_width" \
  METALXR_VIEW_HEIGHT="$view_height" \
  METALXR_TRACKING_STATE_PATH="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}" \
  METALXR_HAPTIC_COMMAND_PATH="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}" \
  METALXR_TIMING_STATE_PATH="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}" \
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
  METALXR_TRACKING_STATE_PATH="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}" \
  METALXR_HAPTIC_COMMAND_PATH="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}" \
  METALXR_TIMING_STATE_PATH="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}" \
  "$repo_root/Scripts/run-metalxr-frame-stream.sh" >"$streamer_log" 2>&1 &
  streamer_pid="$!"
}

wait_for_quest_display_logs() {
  local start
  start="$(date +%s)"
  while true; do
    if "$adb_path" logcat -d Unity:D '*:S' | grep -q 'MetalXRQuestClient displayed VIDEO_FRAME'; then
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
launch_quest_client
launch_unity_and_enter_play_mode
wait_for_export_pair
start_unity_export_streamer
wait_for_file_pattern "Quest streamer connection" "$stream_wait_seconds" "$streamer_log" 'Quest stream client connected'
wait_for_file_pattern "streamed VIDEO_FRAME packets" "$stream_wait_seconds" "$streamer_log" '"event":"streamed"'
wait_for_quest_display_logs

METALXR_SCREENSHOT_PATH="$screenshot_path" "$repo_root/Scripts/probe-quest-client-screenshot.sh"

echo "MetalXR Play Mode workflow smoke passed."
echo "Unity log: $unity_log"
echo "Runtime log: $runtime_log"
echo "Frame exports: $frame_export_dir"
echo "Streamer log: $streamer_log"
echo "Quest screenshot: $screenshot_path"
workflow_complete=1

if [[ "$keep_running" == "1" ]]; then
  echo "Unity and host streamer are left running. Press Ctrl-C to stop the foreground streamer wait."
  wait "$streamer_pid"
fi
