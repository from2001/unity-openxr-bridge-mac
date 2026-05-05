#!/usr/bin/env bash
set -euo pipefail

# Compile the Unity Quest client scripts in the running Unity Editor through uloop.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_path="${UNITY_PROJECT_PATH:-$repo_root/TestProjects/UnityOpenXRSmoke}"

if ! command -v uloop >/dev/null 2>&1; then
  echo "uloop was not found on PATH." >&2
  exit 1
fi

uloop compile --project-path "$project_path" --force-recompile false --wait-for-domain-reload true
