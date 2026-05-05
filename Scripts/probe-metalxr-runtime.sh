#!/usr/bin/env bash
set -euo pipefail

# Probe the runtime skeleton by dlopen'ing the dylib and running OpenXR loader
# negotiation with the same structs used by the Khronos loader interface.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$repo_root/Runtime/MetalXRRuntime"
runtime_dylib="$runtime_dir/build/libmetalxr_runtime.dylib"
probe_source="${TMPDIR:-/tmp}/metalxr_runtime_probe.c"
probe_binary="${TMPDIR:-/tmp}/metalxr_runtime_probe"
probe_log="${TMPDIR:-/tmp}/metalxr_runtime_probe.log"

if [[ ! -f "$runtime_dylib" ]]; then
  "$repo_root/Scripts/build-metalxr-runtime.sh"
fi

cat > "$probe_source" <<'PROBE'
#include "MetalXRRuntime/openxr_minimal.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest);

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /path/to/libmetalxr_runtime.dylib\n", argv[0]);
        return 2;
    }

    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(library, "xrNegotiateLoaderRuntimeInterface");
    if (negotiate == NULL) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }

    XrNegotiateLoaderInfo loaderInfo = {
        XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
        XR_LOADER_INFO_STRUCT_VERSION,
        sizeof(XrNegotiateLoaderInfo),
        XR_CURRENT_LOADER_RUNTIME_VERSION,
        XR_CURRENT_LOADER_RUNTIME_VERSION,
        XR_MAKE_VERSION(1, 0, 0),
        XR_MAKE_VERSION(1, 1, 0)
    };

    XrNegotiateRuntimeRequest runtimeRequest = {
        XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
        XR_RUNTIME_INFO_STRUCT_VERSION,
        sizeof(XrNegotiateRuntimeRequest),
        0,
        0,
        NULL
    };

    XrResult result = negotiate(&loaderInfo, &runtimeRequest);
    printf("negotiate=%d interface=%u api=%u.%u getInstanceProcAddr=%s\n",
           result,
           runtimeRequest.runtimeInterfaceVersion,
           XR_VERSION_MAJOR(runtimeRequest.runtimeApiVersion),
           XR_VERSION_MINOR(runtimeRequest.runtimeApiVersion),
           runtimeRequest.getInstanceProcAddr != NULL ? "yes" : "no");

    if (result != XR_SUCCESS || runtimeRequest.getInstanceProcAddr == NULL) {
        return 1;
    }

    PFN_xrVoidFunction createInstance = NULL;
    result = runtimeRequest.getInstanceProcAddr(NULL, "xrCreateInstance", &createInstance);
    printf("getProcAddr(xrCreateInstance)=%d function=%s\n",
           result,
           createInstance != NULL ? "yes" : "no");

    return (result == XR_SUCCESS && createInstance != NULL) ? 0 : 1;
}
PROBE

clang -std=c11 -Wall -Wextra -Werror \
  -I "$runtime_dir/include" \
  "$probe_source" \
  -o "$probe_binary"

rm -f "$probe_log"
METALXR_RUNTIME_LOG="$probe_log" "$probe_binary" "$runtime_dylib"

echo "Runtime log: $probe_log"
sed -n '1,120p' "$probe_log"
