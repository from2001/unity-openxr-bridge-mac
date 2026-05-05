# MetalXR OpenXR Runtime Lifecycle

This document tracks the native runtime milestones for issue #2 and issue #3.

## Purpose

The runtime proves that the OpenXR loader can discover and load a MetalXR runtime on macOS, then exercise enough core OpenXR lifecycle behavior for Unity Play Mode initialization work. It intentionally does not implement real swapchain images, frame capture, tracking, input, or streaming yet.

The current runtime:

- Exports `xrNegotiateLoaderRuntimeInterface`.
- Validates the loader/runtime negotiation structs.
- Returns OpenXR API version 1.0.
- Provides `xrGetInstanceProcAddr`.
- Provides `XR_KHR_metal_enable` and Unity's legacy `XR_KHRX2_metal_enable` extension discovery plus Metal graphics requirements.
- Implements core lifecycle calls for:
  - `xrEnumerateApiLayerProperties`
  - `xrEnumerateInstanceExtensionProperties`
  - `xrCreateInstance`
  - `xrDestroyInstance`
  - `xrGetInstanceProperties`
  - `xrGetSystem`
  - `xrGetSystemProperties`
  - `xrCreateSession`
  - `xrDestroySession`
  - `xrBeginSession`
  - `xrEndSession`
  - `xrRequestExitSession`
  - `xrPollEvent`
  - `xrEnumerateReferenceSpaces`
  - `xrCreateReferenceSpace`
  - `xrDestroySpace`
  - `xrLocateSpace`
  - `xrEnumerateViewConfigurations`
  - `xrGetViewConfigurationProperties`
  - `xrEnumerateViewConfigurationViews`
  - `xrEnumerateEnvironmentBlendModes`
  - `xrWaitFrame`
  - `xrBeginFrame`
  - `xrEndFrame`
  - `xrLocateViews`
- Reports a dummy stereo HMD with two fixed 1832 x 1920 views and opaque environment blending.
- Emits lifecycle logs for instance/session creation, session state transitions, and sampled frame-loop calls.

The runtime currently accepts a Metal graphics binding enough to create a session, but does not create runtime-owned Metal swapchain images. That is the next roadmap item.

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

The probe uses `dlopen`, calls `xrNegotiateLoaderRuntimeInterface`, creates an instance with Unity's `XR_KHRX2_metal_enable` path, discovers the dummy HMD system, creates a session, consumes session-state events, creates a local reference space, and runs three deterministic frames through `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`, and `xrEndFrame`.

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

Issue #4 should prove the Unity macOS graphics path and replace the current no-swapchain lifecycle runtime with a minimal Metal swapchain path that Unity accepts. That work must determine how the runtime can access or capture Unity-rendered stereo frame images.
