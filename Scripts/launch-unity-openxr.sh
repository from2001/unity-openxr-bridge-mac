#!/usr/bin/env bash
set -euo pipefail

# Launch Unity with a specific OpenXR runtime manifest.
# This avoids requiring root access to modify /usr/local/share/openxr/1/active_runtime.json.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/launch-unity-openxr.sh [UNITY_PROJECT_PATH]

Environment:
  METALXR_RUNTIME_JSON     Full path to an OpenXR runtime JSON manifest.
  METALXR_RUNTIME_LOG      Runtime log path. Defaults to TMPDIR/metalxr_unity_runtime.log.
  METALXR_FRAME_DUMP_DIR   Directory for frame metadata dumps from the MetalXR runtime.
  METALXR_FRAME_EXPORT_DIR Directory for per-eye frame export records and payloads.
  METALXR_FRAME_EXPORT_SOCKET
                           Optional datagram socket path for per-eye frame export records.
  METALXR_FRAME_EXPORT_MODE
                           Frame export mode: readback or fixture. Defaults to readback.
  METALXR_SWAPCHAIN_STORAGE_MODE
                           Metal storage mode: shared, managed, or private. Defaults to runtime policy.
  METALXR_VIEW_WIDTH       Runtime eye width. Defaults to runtime fallback.
  METALXR_VIEW_HEIGHT      Runtime eye height. Defaults to runtime fallback.
  METALXR_REFRESH_RATE     Runtime refresh rate in Hz. Defaults to runtime fallback.
  METALXR_PREDICTION_OFFSET_MS
                           Signed prediction offset in milliseconds. Defaults to timing-state value or 0.
  METALXR_TRACKING_STATE_PATH
                           HMD/controller state file. Defaults to /tmp/metalxr_tracking_state.txt.
  METALXR_HAPTIC_COMMAND_PATH
                           Haptic command file. Defaults to /tmp/metalxr_haptic_command.txt.
  METALXR_TIMING_STATE_PATH
                           Timing state file. Defaults to /tmp/metalxr_timing_state.txt.
  UNITY_APP                Full path to Unity.app. If omitted, the project version is used first.
  METALXR_START_SIMULATOR  Set to 0 to avoid launching MetaXRSimulator.app automatically.
  METALXR_START_ULOOP_SERVER
                           Set to 1 to start Unity CLI Loop after project load.
  XR_LOADER_DEBUG          OpenXR loader logging level. Defaults to warn.
USAGE
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_path="${1:-}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

metalxr_runtime_manifest="$repo_root/Runtime/MetalXRRuntime/metalxr_runtime.json"
metalxr_runtime_dylib="$repo_root/Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib"
meta_simulator_manifest="/Applications/MetaXRSimulator.app/Contents/Resources/MetaXRSimulator/meta_openxr_simulator.json"
system_runtime_manifest="/usr/local/share/openxr/1/active_runtime.json"
user_runtime_manifest="${HOME}/.config/openxr/1/active_runtime.json"

runtime_json="${METALXR_RUNTIME_JSON:-}"
if [[ -z "$runtime_json" && -f "$metalxr_runtime_manifest" && -f "$metalxr_runtime_dylib" ]]; then
  runtime_json="$metalxr_runtime_manifest"
fi
if [[ -z "$runtime_json" && -f "$meta_simulator_manifest" ]]; then
  runtime_json="$meta_simulator_manifest"
fi
if [[ -z "$runtime_json" && -e "$user_runtime_manifest" ]]; then
  runtime_json="$user_runtime_manifest"
fi
if [[ -z "$runtime_json" && -e "$system_runtime_manifest" ]]; then
  runtime_json="$system_runtime_manifest"
fi

if [[ -z "$runtime_json" || ! -e "$runtime_json" ]]; then
  echo "No OpenXR runtime manifest found." >&2
  echo "Install Meta XR Simulator or set METALXR_RUNTIME_JSON to a runtime JSON file." >&2
  exit 1
fi

unity_app="${UNITY_APP:-}"
if [[ -z "$unity_app" ]]; then
  if [[ -n "$project_path" && -f "$project_path/ProjectSettings/ProjectVersion.txt" ]]; then
    project_unity_version="$(
      awk -F': ' '/^m_EditorVersion:/ { print $2; exit }' "$project_path/ProjectSettings/ProjectVersion.txt"
    )"
    if [[ -n "$project_unity_version" && -x "/Applications/Unity/Hub/Editor/$project_unity_version/Unity.app/Contents/MacOS/Unity" ]]; then
      unity_app="/Applications/Unity/Hub/Editor/$project_unity_version/Unity.app"
    fi
  fi
fi
if [[ -z "$unity_app" ]]; then
  unity_app="$(find /Applications/Unity/Hub/Editor -maxdepth 2 -name Unity.app -type d 2>/dev/null | sort | tail -n 1 || true)"
fi

if [[ -z "$unity_app" || ! -x "$unity_app/Contents/MacOS/Unity" ]]; then
  echo "Unity.app was not found. Set UNITY_APP=/path/to/Unity.app." >&2
  exit 1
fi

if [[ "${METALXR_START_SIMULATOR:-1}" != "0" && "$runtime_json" == "$meta_simulator_manifest" ]]; then
  open -a MetaXRSimulator || true
fi

unity_args=()
if [[ -n "$project_path" ]]; then
  if [[ ! -d "$project_path" ]]; then
    echo "Unity project path does not exist: $project_path" >&2
    exit 1
  fi
  unity_args+=("-projectPath" "$project_path")
fi

if [[ "${METALXR_START_ULOOP_SERVER:-0}" == "1" ]]; then
  unity_args+=("-executeMethod" "MetalXR.Editor.MetalXRWorkflowAutomation.StartUnityCliLoopServer")
fi

echo "Repository: $repo_root"
echo "Unity: $unity_app"
echo "OpenXR runtime: $runtime_json"

metalxr_runtime_log="${METALXR_RUNTIME_LOG:-${TMPDIR:-/tmp}/metalxr_unity_runtime.log}"
metalxr_frame_dump_dir="${METALXR_FRAME_DUMP_DIR:-${TMPDIR:-/tmp}/metalxr_unity_frames}"
metalxr_frame_export_dir="${METALXR_FRAME_EXPORT_DIR:-}"
metalxr_frame_export_socket="${METALXR_FRAME_EXPORT_SOCKET:-}"
metalxr_frame_export_mode="${METALXR_FRAME_EXPORT_MODE:-readback}"
metalxr_swapchain_storage_mode="${METALXR_SWAPCHAIN_STORAGE_MODE:-}"
metalxr_view_width="${METALXR_VIEW_WIDTH:-}"
metalxr_view_height="${METALXR_VIEW_HEIGHT:-}"
metalxr_tracking_state_path="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}"
metalxr_haptic_command_path="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}"
metalxr_timing_state_path="${METALXR_TIMING_STATE_PATH:-/tmp/metalxr_timing_state.txt}"
mkdir -p "$metalxr_frame_dump_dir"
if [[ -n "$metalxr_frame_export_dir" ]]; then
  mkdir -p "$metalxr_frame_export_dir"
fi

echo "MetalXR runtime log: $metalxr_runtime_log"
echo "MetalXR frame dumps: $metalxr_frame_dump_dir"
if [[ -n "$metalxr_frame_export_dir" ]]; then
  echo "MetalXR frame exports: $metalxr_frame_export_dir"
  echo "MetalXR frame export mode: $metalxr_frame_export_mode"
fi
if [[ -n "$metalxr_frame_export_socket" ]]; then
  echo "MetalXR frame export socket: $metalxr_frame_export_socket"
fi
if [[ -n "$metalxr_swapchain_storage_mode" ]]; then
  echo "MetalXR swapchain storage mode: $metalxr_swapchain_storage_mode"
fi
if [[ -n "$metalxr_view_width" || -n "$metalxr_view_height" ]]; then
  echo "MetalXR view size: ${metalxr_view_width:-runtime}x${metalxr_view_height:-runtime}"
fi
echo "MetalXR tracking state: $metalxr_tracking_state_path"
echo "MetalXR haptic commands: $metalxr_haptic_command_path"
echo "MetalXR timing state: $metalxr_timing_state_path"

XR_RUNTIME_JSON="$runtime_json" \
XR_LOADER_DEBUG="${XR_LOADER_DEBUG:-warn}" \
METALXR_RUNTIME_LOG="$metalxr_runtime_log" \
METALXR_FRAME_DUMP_DIR="$metalxr_frame_dump_dir" \
METALXR_FRAME_EXPORT_DIR="$metalxr_frame_export_dir" \
METALXR_FRAME_EXPORT_SOCKET="$metalxr_frame_export_socket" \
METALXR_FRAME_EXPORT_MODE="$metalxr_frame_export_mode" \
METALXR_SWAPCHAIN_STORAGE_MODE="$metalxr_swapchain_storage_mode" \
METALXR_VIEW_WIDTH="$metalxr_view_width" \
METALXR_VIEW_HEIGHT="$metalxr_view_height" \
METALXR_START_ULOOP_SERVER="${METALXR_START_ULOOP_SERVER:-0}" \
METALXR_TRACKING_STATE_PATH="$metalxr_tracking_state_path" \
METALXR_HAPTIC_COMMAND_PATH="$metalxr_haptic_command_path" \
METALXR_TIMING_STATE_PATH="$metalxr_timing_state_path" \
"$unity_app/Contents/MacOS/Unity" "${unity_args[@]}"
