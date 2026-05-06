#!/usr/bin/env bash
set -euo pipefail

# Build the Android ARM64 Unity native plugin used by the Quest SurfaceTexture path.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/build-quest-gl-plugin.sh

Environment:
  UNITY_APP  Full path to Unity.app. If omitted, the Unity version from the smoke project is used.
  ANDROID_NDK_HOME  Optional Android NDK path. Defaults to Unity's bundled Android NDK.
  METALXR_QUEST_GL_PLUGIN_OUTPUT  Optional output .so path.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_path="${UNITY_PROJECT_PATH:-$repo_root/TestProjects/UnityOpenXRSmoke}"
source_path="$repo_root/Runtime/MetalXRQuestGLPlugin/src/metalxr_quest_gl_plugin.cpp"
output_path="${METALXR_QUEST_GL_PLUGIN_OUTPUT:-$project_path/Assets/Plugins/Android/libs/arm64-v8a/libmetalxrquestgl.so}"

if [[ "$output_path" != /* ]]; then
  output_path="$repo_root/$output_path"
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

if [[ -z "$unity_app" || ! -d "$unity_app/Contents/PluginAPI" ]]; then
  echo "Unity PluginAPI headers were not found. Set UNITY_APP=/path/to/Unity.app." >&2
  exit 1
fi

ndk_path="${ANDROID_NDK_HOME:-}"
if [[ -z "$ndk_path" ]]; then
  ndk_path="$(dirname "$unity_app")/PlaybackEngines/AndroidPlayer/NDK"
fi

if [[ ! -d "$ndk_path/toolchains/llvm/prebuilt" ]]; then
  echo "Android NDK was not found. Set ANDROID_NDK_HOME=/path/to/ndk." >&2
  exit 1
fi

clangxx="$(find "$ndk_path/toolchains/llvm/prebuilt" -path '*/bin/aarch64-linux-android29-clang++' -type f | head -n 1 || true)"
if [[ -z "$clangxx" ]]; then
  clangxx="$(find "$ndk_path/toolchains/llvm/prebuilt" -path '*/bin/aarch64-linux-android*-clang++' -type f | sort | head -n 1 || true)"
fi

if [[ -z "$clangxx" || ! -x "$clangxx" ]]; then
  echo "Android ARM64 clang++ was not found in $ndk_path." >&2
  exit 1
fi

mkdir -p "$(dirname "$output_path")"

"$clangxx" \
  -std=c++17 \
  -fPIC \
  -shared \
  -O2 \
  -Wall \
  -Wextra \
  -I"$unity_app/Contents/PluginAPI" \
  "$source_path" \
  -o "$output_path" \
  -llog \
  -lGLESv2

echo "Built $output_path"
