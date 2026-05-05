#!/usr/bin/env bash
set -euo pipefail

# Build the macOS host-side MetalXR streaming utilities.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host_dir="$repo_root/Runtime/MetalXRHost"
build_dir="$host_dir/build"

clang_bin="$(command -v clang || true)"
if [[ -z "$clang_bin" ]]; then
  clang_bin="$(xcrun --find clang 2>/dev/null || true)"
fi

if [[ -z "$clang_bin" ]]; then
  echo "clang is required to build the MetalXR host utilities." >&2
  exit 1
fi

if command -v cmake >/dev/null 2>&1; then
  cmake -S "$host_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug >/dev/null
  cmake --build "$build_dir" --target metalxr_host_encoder >/dev/null
  cmake --build "$build_dir" --target metalxr_host_streamer >/dev/null
else
  protocol_dir="$repo_root/Runtime/MetalXRProtocol"
  mkdir -p "$build_dir"
  "$clang_bin" -std=c11 -Wall -Wextra -Werror \
    "$host_dir/src/metalxr_host_encoder.c" \
    -framework CoreFoundation \
    -framework CoreMedia \
    -framework CoreVideo \
    -framework VideoToolbox \
    -o "$build_dir/metalxr_host_encoder"
  "$clang_bin" -std=c11 -Wall -Wextra -Werror \
    -I "$protocol_dir/include" \
    "$protocol_dir/src/metalxr_protocol.c" \
    "$host_dir/src/metalxr_host_streamer.c" \
    -framework CoreFoundation \
    -framework CoreMedia \
    -framework CoreVideo \
    -framework VideoToolbox \
    -o "$build_dir/metalxr_host_streamer"
fi

echo "Built $build_dir/metalxr_host_encoder"
echo "Built $build_dir/metalxr_host_streamer"
