# MetalXR OpenXR Runtime Skeleton

This document tracks the first native runtime milestone for issue #2.

## Purpose

The skeleton runtime proves that the OpenXR loader can discover and load a MetalXR runtime on macOS. It intentionally does not implement an HMD, swapchains, frame timing, tracking, input, or streaming yet.

The current runtime:

- Exports `xrNegotiateLoaderRuntimeInterface`.
- Validates the loader/runtime negotiation structs.
- Returns OpenXR API version 1.0.
- Provides `xrGetInstanceProcAddr`.
- Provides minimal stubs for:
  - `xrEnumerateApiLayerProperties`
  - `xrEnumerateInstanceExtensionProperties`
  - `xrCreateInstance`
- Returns `XR_ERROR_RUNTIME_UNAVAILABLE` from `xrCreateInstance` until issue #3 adds lifecycle support.

## Build

```sh
Scripts/build-metalxr-runtime.sh
```

The dylib is built at:

```text
Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib
```

The runtime manifest is:

```text
Runtime/MetalXRRuntime/metalxr_runtime.json
```

## Probe

```sh
Scripts/probe-metalxr-runtime.sh
```

The probe uses `dlopen`, calls `xrNegotiateLoaderRuntimeInterface`, and confirms that `xrGetInstanceProcAddr` returns a function pointer for `xrCreateInstance`.

## Unity Launch

After the runtime is built, this script will prefer the MetalXR runtime manifest over Meta XR Simulator:

```sh
Scripts/launch-unity-openxr.sh /path/to/UnityProject
```

For a one-off override:

```sh
METALXR_RUNTIME_JSON=/absolute/path/to/runtime.json Scripts/launch-unity-openxr.sh /path/to/UnityProject
```

## Next Step

Issue #3 should replace the intentional `XR_ERROR_RUNTIME_UNAVAILABLE` response with enough OpenXR lifecycle behavior for Unity Play Mode to create an instance, discover a dummy HMD system, and enter a controlled frame loop.
