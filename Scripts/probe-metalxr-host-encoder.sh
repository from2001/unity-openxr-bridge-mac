#!/usr/bin/env bash
set -euo pipefail

# Probe the host encoder with a short synthetic stereo frame stream.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
encoder="$repo_root/Runtime/MetalXRHost/build/metalxr_host_encoder"
output_prefix="${TMPDIR:-/tmp}/metalxr_host_encoder_probe"

if [[ ! -x "$encoder" ]]; then
  "$repo_root/Scripts/build-metalxr-host.sh"
fi

rm -f "${output_prefix}_left.h264" \
      "${output_prefix}_right.h264" \
      "${output_prefix}_metadata.jsonl"

"$encoder" \
  --output-prefix "$output_prefix" \
  --frames 30 \
  --fps 30 \
  --width 320 \
  --height 180 \
  --bitrate 2000000 \
  --realtime

for output in "${output_prefix}_left.h264" "${output_prefix}_right.h264" "${output_prefix}_metadata.jsonl"; do
  if [[ ! -s "$output" ]]; then
    echo "Expected encoder output was not written: $output" >&2
    exit 1
  fi
done

encoded_count="$(grep -c '"event":"encoded"' "${output_prefix}_metadata.jsonl" || true)"
summary_count="$(grep -c '"event":"summary"' "${output_prefix}_metadata.jsonl" || true)"
drop_count="$(grep -c '"event":"drop"' "${output_prefix}_metadata.jsonl" || true)"

if [[ "$encoded_count" -ne 60 ]]; then
  echo "Expected 60 encoded eye frames, got $encoded_count" >&2
  exit 1
fi

if [[ "$summary_count" -ne 2 ]]; then
  echo "Expected 2 summary records, got $summary_count" >&2
  exit 1
fi

if [[ "$drop_count" -ne 0 ]]; then
  echo "Expected no dropped frames, got $drop_count" >&2
  exit 1
fi

echo "Host encoder metadata: ${output_prefix}_metadata.jsonl"
sed -n '1,8p' "${output_prefix}_metadata.jsonl"
tail -2 "${output_prefix}_metadata.jsonl"
