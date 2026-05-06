#!/usr/bin/env bash
set -euo pipefail

# Build the Unity OpenXR smoke project as a Quest/Android APK.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/build-quest-client-apk.sh [OUTPUT_APK]

Environment:
  UNITY_APP                    Full path to Unity.app. If omitted, the newest Unity Hub editor is used.
  UNITY_PROJECT_PATH           Unity project path. Defaults to TestProjects/UnityOpenXRSmoke.
  METALXR_QUEST_BUILD_LOG      Unity batchmode log path. Defaults to TMPDIR/metalxr_quest_client_build.log.
  METALXR_QUEST_GRAPHICS_API   Optional Android graphics API override: gles3, vulkan, or comma-separated order.
  METALXR_QUEST_BUILD_GL_PLUGIN Build the Android GL native plugin before the APK. Defaults to 1.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_path="${UNITY_PROJECT_PATH:-$repo_root/TestProjects/UnityOpenXRSmoke}"
output_apk="${1:-$project_path/Builds/MetalXRQuestClient.apk}"
build_log="${METALXR_QUEST_BUILD_LOG:-${TMPDIR:-/tmp}/metalxr_quest_client_build.log}"
quest_graphics_api="${METALXR_QUEST_GRAPHICS_API:-}"
build_gl_plugin="${METALXR_QUEST_BUILD_GL_PLUGIN:-1}"

if [[ "$output_apk" != /* ]]; then
  output_apk="$repo_root/$output_apk"
fi

if [[ ! -d "$project_path" ]]; then
  echo "Unity project path does not exist: $project_path" >&2
  exit 1
fi

unity_app="${UNITY_APP:-}"
if [[ -z "$unity_app" ]]; then
  project_unity_version="$(awk '/^m_EditorVersion:/{print $2; exit}' "$project_path/ProjectSettings/ProjectVersion.txt" 2>/dev/null || true)"
  if [[ -n "$project_unity_version" && -x "/Applications/Unity/Hub/Editor/$project_unity_version/Unity.app/Contents/MacOS/Unity" ]]; then
    unity_app="/Applications/Unity/Hub/Editor/$project_unity_version/Unity.app"
  else
    unity_app="$(find /Applications/Unity/Hub/Editor -maxdepth 2 -name Unity.app -type d 2>/dev/null | sort | tail -n 1 || true)"
  fi
fi

if [[ -z "$unity_app" || ! -x "$unity_app/Contents/MacOS/Unity" ]]; then
  echo "Unity.app was not found. Set UNITY_APP=/path/to/Unity.app." >&2
  exit 1
fi

mkdir -p "$(dirname "$output_apk")" "$(dirname "$build_log")"

echo "Repository: $repo_root"
echo "Unity: $unity_app"
echo "Project: $project_path"
echo "Output APK: $output_apk"
echo "Build log: $build_log"

if [[ "$build_gl_plugin" == "1" ]]; then
  "$repo_root/Scripts/build-quest-gl-plugin.sh"
fi

if [[ -n "$quest_graphics_api" ]]; then
  build_command=(
    "$unity_app/Contents/MacOS/Unity"
    -batchmode
    -quit
    -nographics
    -projectPath "$project_path"
    -buildTarget Android
    -executeMethod MetalXRQuestClientBuild.BuildAndroidApk
    -metalxrBuildOutput "$output_apk"
    -metalxrAndroidGraphicsApi "$quest_graphics_api"
    -logFile "$build_log"
  )
else
  build_command=(
    "$unity_app/Contents/MacOS/Unity"
    -batchmode
    -quit
    -nographics
    -projectPath "$project_path"
    -buildTarget Android
    -executeMethod MetalXRQuestClientBuild.BuildAndroidApk
    -metalxrBuildOutput "$output_apk"
    -logFile "$build_log"
  )
fi

if ! "${build_command[@]}"; then
  echo "Unity APK build failed. Last log lines:" >&2
  tail -n 120 "$build_log" >&2 || true
  exit 1
fi

echo "Built $output_apk"
