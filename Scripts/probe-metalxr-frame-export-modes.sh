#!/usr/bin/env bash
set -euo pipefail

# Compare development readback export with the guarded IOSurface socket export path.

usage() {
  cat <<'USAGE'
Usage:
  Scripts/probe-metalxr-frame-export-modes.sh

Environment:
  METALXR_FRAME_EXPORT_COMPARE_DIR    Output directory. Defaults to TMPDIR/metalxr_frame_export_compare_TIMESTAMP.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
timestamp="$(date +%Y%m%d_%H%M%S)"
tmp_root="${TMPDIR:-/tmp}"
output_root="${METALXR_FRAME_EXPORT_COMPARE_DIR:-$tmp_root/metalxr_frame_export_compare_$timestamp}"
readback_root="$output_root/readback"
iosurface_root="$output_root/iosurface_socket"
summary_path="$output_root/summary.txt"
socket_prefix="${METALXR_FRAME_EXPORT_COMPARE_SOCKET_PREFIX:-/tmp/mxr_fecmp_${timestamp}_$$}"
iosurface_socket_path="${socket_prefix}.sock"
iosurface_ack_socket_path="${socket_prefix}.ack"

mkdir -p "$readback_root/tmp" "$iosurface_root/tmp"

run_readback_probe() {
  local log_path="$readback_root/probe.log"
  TMPDIR="$readback_root/tmp" \
  METALXR_PROBE_FRAME_EXPORT_DIR="$readback_root/frame_export" \
  METALXR_PROBE_FRAME_EXPORT_MODE=readback \
  "$repo_root/Scripts/probe-metalxr-runtime.sh" >"$log_path" 2>&1
}

run_iosurface_socket_probe() {
  local log_path="$iosurface_root/probe.log"
  rm -f "$iosurface_socket_path" "$iosurface_ack_socket_path"
  TMPDIR="$iosurface_root/tmp" \
  METALXR_PROBE_FRAME_EXPORT_DIR= \
  METALXR_PROBE_FRAME_EXPORT_SOCKET="$iosurface_socket_path" \
  METALXR_PROBE_FRAME_EXPORT_ACK_SOCKET="$iosurface_ack_socket_path" \
  METALXR_PROBE_FRAME_EXPORT_MODE=iosurface \
  METALXR_PROBE_SWAPCHAIN_RESOURCE_MODE=iosurface \
  METALXR_PROBE_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1 \
  "$repo_root/Scripts/probe-metalxr-runtime.sh" >"$log_path" 2>&1
}

json_string_field() {
  local line="$1"
  local key="$2"
  printf '%s\n' "$line" | sed -nE "s/.*\"$key\":\"([^\"]*)\".*/\\1/p" | tail -1
}

json_number_field() {
  local line="$1"
  local key="$2"
  local value
  value="$(printf '%s\n' "$line" | sed -nE "s/.*\"$key\":([0-9]+).*/\\1/p" | tail -1)"
  printf '%s' "${value:-0}"
}

record_line_for_eye() {
  local record_source="$1"
  local eye="$2"
  grep "\"eye\":$eye" "$record_source" | head -1
}

summarize_readback_eye() {
  local eye="$1"
  local record_path="$readback_root/frame_export/frame_000001_eye_${eye}.json"
  local payload_path="$readback_root/frame_export/frame_000001_eye_${eye}.bgra"
  if [[ ! -s "$record_path" || ! -s "$payload_path" ]]; then
    echo "readback eye=$eye missing record or payload" >&2
    return 1
  fi

  local line
  line="$(cat "$record_path")"
  local payload_format payload_bytes payload_size mode
  payload_format="$(json_string_field "$line" "payloadFormat")"
  payload_bytes="$(json_number_field "$line" "payloadBytes")"
  mode="$(json_string_field "$line" "mode")"
  payload_size="$(wc -c <"$payload_path" | tr -d ' ')"
  if [[ "$payload_format" != "BGRA8" || "$payload_bytes" == "0" || "$payload_size" == "0" ]]; then
    echo "readback eye=$eye invalid payload format=$payload_format payload_bytes=$payload_bytes size=$payload_size" >&2
    return 1
  fi

  printf 'readback eye=%s mode=%s payloadFormat=%s payloadBytes=%s payloadFileBytes=%s payloadPath=%s\n' \
    "$eye" "$mode" "$payload_format" "$payload_bytes" "$payload_size" "$payload_path"
}

summarize_iosurface_eye() {
  local eye="$1"
  local records_path="$iosurface_root/tmp/metalxr_runtime_probe_socket_records.jsonl"
  if [[ ! -s "$records_path" ]]; then
    echo "iosurface eye=$eye missing socket records" >&2
    return 1
  fi

  local line
  line="$(record_line_for_eye "$records_path" "$eye")"
  if [[ -z "$line" ]]; then
    echo "iosurface eye=$eye missing socket record" >&2
    return 1
  fi

  local payload_format payload_bytes io_surface_id frame_slot_generation mode payload_path
  payload_format="$(json_string_field "$line" "payloadFormat")"
  payload_bytes="$(json_number_field "$line" "payloadBytes")"
  io_surface_id="$(json_number_field "$line" "ioSurfaceId")"
  frame_slot_generation="$(json_number_field "$line" "frameSlotGeneration")"
  mode="$(json_string_field "$line" "mode")"
  payload_path="$(json_string_field "$line" "payloadPath")"
  if [[ "$payload_format" != IOSurface* ||
        "$payload_bytes" != "0" ||
        "$io_surface_id" == "0" ||
        "$frame_slot_generation" == "0" ||
        -n "$payload_path" ]]; then
    echo "iosurface eye=$eye invalid record format=$payload_format payload_bytes=$payload_bytes ioSurfaceId=$io_surface_id generation=$frame_slot_generation payloadPath=$payload_path" >&2
    return 1
  fi

  printf 'iosurface eye=%s mode=%s payloadFormat=%s payloadBytes=%s ioSurfaceId=%s frameSlotGeneration=%s socketRecords=%s\n' \
    "$eye" "$mode" "$payload_format" "$payload_bytes" "$io_surface_id" "$frame_slot_generation" "$records_path"
}

run_readback_probe
run_iosurface_socket_probe

{
  echo "MetalXR frame export mode comparison"
  echo "output=$output_root"
  summarize_readback_eye 0
  summarize_readback_eye 1
  summarize_iosurface_eye 0
  summarize_iosurface_eye 1
  echo "readbackLog=$readback_root/probe.log"
  echo "iosurfaceLog=$iosurface_root/probe.log"
} | tee "$summary_path"

echo "Frame export mode comparison passed. Summary: $summary_path"
