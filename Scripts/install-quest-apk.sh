#!/usr/bin/env bash
set -euo pipefail

# Install a Quest/Android APK through the adb binary bundled with this repository.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/install-quest-apk.sh PATH_TO_APK

Environment:
  ADB     Optional adb executable path. Defaults to Libs/adb-lib/adb, then PATH.
  ANDROID_SERIAL  Optional adb serial to target when multiple devices are connected.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || $# -ne 1 ]]; then
  usage
  exit $([[ $# -eq 1 ]] && echo 0 || echo 1)
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
apk_path="$1"

if [[ ! -f "$apk_path" ]]; then
  echo "APK file does not exist: $apk_path" >&2
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

"$adb_path" start-server >/dev/null

if [[ -n "${ANDROID_SERIAL:-}" ]]; then
  device_state="$(adb_cmd get-state 2>/dev/null || true)"
  if [[ "$device_state" != "device" ]]; then
    echo "The requested Android device is not authorized or connected: $ANDROID_SERIAL" >&2
    "$adb_path" devices -l >&2
    exit 1
  fi
else
  devices="$("$adb_path" devices | awk 'NR > 1 && $2 == "device" { print $1 }')"

  if [[ -z "$devices" ]]; then
    echo "No authorized Quest/Android device is connected." >&2
    echo "Connect the headset over USB and accept the USB debugging prompt in the headset." >&2
    "$adb_path" devices -l >&2
    exit 1
  fi

  device_count="$(printf '%s\n' "$devices" | sed '/^$/d' | wc -l | tr -d ' ')"
  if [[ "$device_count" != "1" ]]; then
    echo "More than one device is connected. Set ANDROID_SERIAL before running this script." >&2
    "$adb_path" devices -l >&2
    exit 1
  fi
fi

echo "Installing $apk_path"
adb_cmd install -r "$apk_path"
