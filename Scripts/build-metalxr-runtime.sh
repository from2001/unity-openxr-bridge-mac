#!/usr/bin/env bash
set -euo pipefail

# Build the native MetalXR OpenXR runtime.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$repo_root/Runtime/MetalXRRuntime"
build_dir="$runtime_dir/build"

if command -v cmake >/dev/null 2>&1; then
  cmake -S "$runtime_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"
  cmake --build "$build_dir" --config "${CMAKE_BUILD_TYPE:-Debug}"
else
  if ! command -v clang >/dev/null 2>&1; then
    echo "cmake or clang is required to build the MetalXR runtime." >&2
    exit 1
  fi

  mkdir -p "$build_dir"
  clang -std=c11 -Wall -Wextra -Werror \
    -dynamiclib \
    -fvisibility=hidden \
    -I "$runtime_dir/include" \
    "$runtime_dir/src/metalxr_runtime.c" \
    -install_name "@rpath/libmetalxr_runtime.dylib" \
    -o "$build_dir/libmetalxr_runtime.dylib"
fi

dylib="$build_dir/libmetalxr_runtime.dylib"
if [[ ! -f "$dylib" ]]; then
  echo "Build completed but runtime dylib was not found: $dylib" >&2
  exit 1
fi

echo "Built $dylib"
nm -gU "$dylib" | grep -E 'xrNegotiateLoaderRuntimeInterface|xrGetInstanceProcAddr'
