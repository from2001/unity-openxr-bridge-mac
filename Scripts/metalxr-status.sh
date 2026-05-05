#!/usr/bin/env bash
set -euo pipefail

# Print the local state that matters for macOS Unity OpenXR development.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
adb_path="${ADB:-}"
if [[ -z "$adb_path" && -x "$repo_root/Libs/adb-lib/adb" ]]; then
  adb_path="$repo_root/Libs/adb-lib/adb"
fi
if [[ -z "$adb_path" ]]; then
  adb_path="$(command -v adb || true)"
fi

print_runtime() {
  local label="$1"
  local path="$2"

  if [[ -e "$path" ]]; then
    local resolved="$path"
    if [[ -L "$path" ]]; then
      resolved="$(readlink "$path")"
    fi
    echo "$label: $path -> $resolved"
  else
    echo "$label: not found"
  fi
}

echo "MetalXR status"
echo "Repository: $repo_root"
echo

if [[ -n "$adb_path" && -x "$adb_path" ]]; then
  echo "adb: $adb_path"
  "$adb_path" devices -l || true
else
  echo "adb: not found"
fi

echo
echo "OpenXR runtime manifests"
echo "XR_RUNTIME_JSON: ${XR_RUNTIME_JSON:-not set}"
print_runtime "User active runtime" "${HOME}/.config/openxr/1/active_runtime.json"
print_runtime "System active runtime" "/usr/local/share/openxr/1/active_runtime.json"
print_runtime "Meta XR Simulator" "/Applications/MetaXRSimulator.app/Contents/Resources/MetaXRSimulator/meta_openxr_simulator.json"

echo
if [[ -d /Applications/Unity/Hub/Editor ]]; then
  echo "Unity editors"
  find /Applications/Unity/Hub/Editor -maxdepth 2 -name Unity.app -type d 2>/dev/null | sort
else
  echo "Unity editors: Unity Hub editor folder was not found"
fi
