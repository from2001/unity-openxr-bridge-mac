#!/usr/bin/env bash
set -euo pipefail

# Install and launch the MetalXR Quest client APK, then print recent Unity logcat lines.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/install-run-quest-client.sh [APK_PATH]

Environment:
  ADB                         Optional adb executable path. Defaults to Libs/adb-lib/adb, then PATH.
  METALXR_QUEST_PACKAGE       Android package name. Defaults to com.metalxr.questclient.
  METALXR_QUEST_ACTIVITY      Android activity class. Defaults to UnityPlayerGameActivity.
  METALXR_QUEST_LAUNCH_CATEGORY
                              Android launch category. Defaults to com.oculus.intent.category.VR.
  METALXR_QUEST_SURFACE_DECODE
                              Pass the experimental Surface decode opt-in to Quest. Defaults to 0.
  METALXR_QUEST_SURFACE_PRESENT
                              Pass the experimental Surface external-texture presentation opt-in to Quest. Defaults to 0.
  METALXR_QUEST_PRESENTATION_MODE
                              Quest presentation mode: projection or diagnostic. Defaults to projection.
  METALXR_QUEST_RENDER_SCALE  Quest XR render scale for the presentation client. Defaults to app-side 1.2.
  METALXR_HOST_PORT           Host handshake port forwarded with adb reverse. Defaults to 47000.
  METALXR_LOGCAT_WAIT_SECONDS Seconds to wait for Unity startup logs. Defaults to 10.
  METALXR_LOGCAT_FOLLOW       Set to 1 to keep following logcat after launch.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
apk_path="${1:-$repo_root/TestProjects/UnityOpenXRSmoke/Builds/MetalXRQuestClient.apk}"
package_name="${METALXR_QUEST_PACKAGE:-com.metalxr.questclient}"
quest_activity="${METALXR_QUEST_ACTIVITY:-com.unity3d.player.UnityPlayerGameActivity}"
launch_category="${METALXR_QUEST_LAUNCH_CATEGORY:-com.oculus.intent.category.VR}"
quest_surface_decode="${METALXR_QUEST_SURFACE_DECODE:-0}"
quest_surface_present="${METALXR_QUEST_SURFACE_PRESENT:-0}"
quest_presentation_mode="${METALXR_QUEST_PRESENTATION_MODE:-}"
quest_render_scale="${METALXR_QUEST_RENDER_SCALE:-}"
host_port="${METALXR_HOST_PORT:-47000}"
logcat_wait_seconds="${METALXR_LOGCAT_WAIT_SECONDS:-10}"

if [[ ! -f "$apk_path" ]]; then
  echo "APK file does not exist: $apk_path" >&2
  echo "Build it first with Scripts/build-quest-client-apk.sh." >&2
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

"$repo_root/Scripts/install-quest-apk.sh" "$apk_path"

echo "Forwarding device tcp:$host_port to host tcp:$host_port"
adb_cmd reverse "tcp:$host_port" "tcp:$host_port" || true

echo "Clearing logcat"
adb_cmd logcat -c || true

echo "Launching $package_name/$quest_activity with $launch_category"
start_args=(
  shell am start -W
  -a android.intent.action.MAIN
  -c "$launch_category"
  -n "$package_name/$quest_activity"
)
if [[ "$quest_surface_decode" == "1" ]]; then
  start_args+=(--ez metalxr_surface_decode true)
fi
if [[ "$quest_surface_present" == "1" ]]; then
  start_args+=(--ez metalxr_surface_present true)
fi
if [[ -n "$quest_presentation_mode" ]]; then
  start_args+=(--es metalxr_presentation_mode "$quest_presentation_mode")
fi
if [[ -n "$quest_render_scale" ]]; then
  start_args+=(--ef metalxr_render_scale "$quest_render_scale")
fi
adb_cmd "${start_args[@]}"

sleep "$logcat_wait_seconds"
echo "Recent MetalXRQuestClient logs:"
adb_cmd logcat -d Unity:D '*:S' | grep 'MetalXRQuestClient' || true

if [[ "${METALXR_LOGCAT_FOLLOW:-0}" == "1" ]]; then
  echo "Following Unity logcat. Press Ctrl-C to stop."
  adb_cmd logcat Unity:D '*:S' | grep --line-buffered 'MetalXRQuestClient'
fi
