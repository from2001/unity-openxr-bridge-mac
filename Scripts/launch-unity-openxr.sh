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
  METALXR_TRACKING_STATE_PATH
                           HMD/controller state file. Defaults to /tmp/metalxr_tracking_state.txt.
  METALXR_HAPTIC_COMMAND_PATH
                           Haptic command file. Defaults to /tmp/metalxr_haptic_command.txt.
  UNITY_APP                Full path to Unity.app. If omitted, the newest Unity Hub editor is used.
  METALXR_START_SIMULATOR  Set to 0 to avoid launching MetaXRSimulator.app automatically.
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

echo "Repository: $repo_root"
echo "Unity: $unity_app"
echo "OpenXR runtime: $runtime_json"

metalxr_runtime_log="${METALXR_RUNTIME_LOG:-${TMPDIR:-/tmp}/metalxr_unity_runtime.log}"
metalxr_frame_dump_dir="${METALXR_FRAME_DUMP_DIR:-${TMPDIR:-/tmp}/metalxr_unity_frames}"
metalxr_tracking_state_path="${METALXR_TRACKING_STATE_PATH:-/tmp/metalxr_tracking_state.txt}"
metalxr_haptic_command_path="${METALXR_HAPTIC_COMMAND_PATH:-/tmp/metalxr_haptic_command.txt}"
mkdir -p "$metalxr_frame_dump_dir"

echo "MetalXR runtime log: $metalxr_runtime_log"
echo "MetalXR frame dumps: $metalxr_frame_dump_dir"
echo "MetalXR tracking state: $metalxr_tracking_state_path"
echo "MetalXR haptic commands: $metalxr_haptic_command_path"

XR_RUNTIME_JSON="$runtime_json" \
XR_LOADER_DEBUG="${XR_LOADER_DEBUG:-warn}" \
METALXR_RUNTIME_LOG="$metalxr_runtime_log" \
METALXR_FRAME_DUMP_DIR="$metalxr_frame_dump_dir" \
METALXR_TRACKING_STATE_PATH="$metalxr_tracking_state_path" \
METALXR_HAPTIC_COMMAND_PATH="$metalxr_haptic_command_path" \
"$unity_app/Contents/MacOS/Unity" "${unity_args[@]}"
