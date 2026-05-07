#!/usr/bin/env bash
set -euo pipefail

# Run the coordinated Play Mode workflow repeatedly with isolated log paths.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/probe-metalxr-playmode-workflow-repeat.sh

Environment:
  METALXR_WORKFLOW_REPEAT_COUNT       Number of workflow runs. Defaults to 2.
  METALXR_WORKFLOW_REPEAT_ROOT        Output root for per-run logs and screenshots.
                                      Defaults to TMPDIR/metalxr_workflow_repeat_TIMESTAMP.

All other METALXR_* and Android environment variables are passed through to
Scripts/run-metalxr-playmode-workflow.sh. This probe forces
METALXR_WORKFLOW_KEEP_RUNNING=0 so each run exercises cleanup and restart.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repeat_count="${METALXR_WORKFLOW_REPEAT_COUNT:-2}"
if ! [[ "$repeat_count" =~ ^[0-9]+$ ]] || (( repeat_count < 1 )); then
  echo "METALXR_WORKFLOW_REPEAT_COUNT must be a positive integer, got: $repeat_count" >&2
  exit 2
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
repeat_root="${METALXR_WORKFLOW_REPEAT_ROOT:-${TMPDIR:-/tmp}/metalxr_workflow_repeat_$timestamp}"
mkdir -p "$repeat_root"

for run_index in $(seq 1 "$repeat_count"); do
  run_dir="$repeat_root/run_$run_index"
  mkdir -p "$run_dir"
  echo "MetalXR workflow repeat run $run_index/$repeat_count output=$run_dir"

  METALXR_WORKFLOW_KEEP_RUNNING=0 \
  METALXR_SCREENSHOT_PATH="$run_dir/quest_stream.png" \
  METALXR_RUNTIME_LOG="$run_dir/runtime.log" \
  METALXR_UNITY_LAUNCH_LOG="$run_dir/unity.log" \
  METALXR_STREAMER_LOG="$run_dir/streamer.log" \
  METALXR_FRAME_EXPORT_DIR="$run_dir/frame_export" \
  METALXR_FRAME_DUMP_DIR="$run_dir/frame_dump" \
  "$repo_root/Scripts/run-metalxr-playmode-workflow.sh"
done

echo "MetalXR repeated workflow smoke passed runs=$repeat_count output=$repeat_root"
