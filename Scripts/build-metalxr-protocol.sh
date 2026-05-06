#!/usr/bin/env bash
set -euo pipefail

# Build the shared MetalXR host/client protocol utilities.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
protocol_dir="$repo_root/Runtime/MetalXRProtocol"
build_dir="$protocol_dir/build"

clang_bin="$(command -v clang || true)"
if [[ -z "$clang_bin" ]]; then
  clang_bin="$(xcrun --find clang 2>/dev/null || true)"
fi

if [[ -z "$clang_bin" ]]; then
  echo "clang is required to build the MetalXR protocol utilities." >&2
  exit 1
fi

if command -v cmake >/dev/null 2>&1; then
  cmake -S "$protocol_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug >/dev/null
  cmake --build "$build_dir" --target metalxr_protocol_loopback >/dev/null
else
  mkdir -p "$build_dir"
    "$clang_bin" -std=c11 -Wall -Wextra -Werror \
    -I "$protocol_dir/include" \
    "$protocol_dir/src/metalxr_protocol.c" \
    "$protocol_dir/src/metalxr_shared_state.c" \
    "$protocol_dir/src/metalxr_protocol_loopback.c" \
    -o "$build_dir/metalxr_protocol_loopback"
fi

echo "Built $build_dir/metalxr_protocol_loopback"
