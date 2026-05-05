# MetalXR OpenXR Runtime Lifecycle and Swapchain

This document tracks the native runtime milestones for issue #2 through issue #4.

## Purpose

The runtime proves that the OpenXR loader can discover and load a MetalXR runtime on macOS, exercise enough core OpenXR lifecycle behavior for Unity Play Mode initialization, and accept Unity-rendered stereo frames through runtime-owned Metal swapchain textures. It intentionally does not implement tracking, input, CPU pixel readback, encoding, transport, or Quest display yet.

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
  - `xrEnumerateSwapchainFormats`
  - `xrCreateSwapchain`
  - `xrDestroySwapchain`
  - `xrEnumerateSwapchainImages`
  - `xrAcquireSwapchainImage`
  - `xrWaitSwapchainImage`
  - `xrReleaseSwapchainImage`
  - `xrEnumerateViewConfigurations`
  - `xrGetViewConfigurationProperties`
  - `xrEnumerateViewConfigurationViews`
  - `xrEnumerateEnvironmentBlendModes`
  - `xrWaitFrame`
  - `xrBeginFrame`
  - `xrEndFrame`
  - `xrLocateViews`
- Reports a dummy stereo HMD with two fixed 1832 x 1920 views and opaque environment blending.
- Creates `MTLTexture` objects through `MTLTextureDescriptor` and returns them via `XrSwapchainImageMetalKHR`.
- Parses submitted `XrCompositionLayerProjection` layers in `xrEndFrame`.
- Emits lifecycle logs for instance/session creation, session state transitions, swapchain ownership, and sampled frame-loop calls.

## macOS Graphics Path

The selected path is direct Metal through `XR_KHR_metal_enable`, plus Unity's legacy `XR_KHRX2_metal_enable` function name. Unity passes a Metal command queue through `XrGraphicsBindingMetalKHR` during `xrCreateSession`. The runtime asks that command queue for its Metal device and allocates runtime-owned 2D Metal textures for swapchain images.

The runtime currently supports single-sample 2D swapchains with these Metal pixel formats:

- `MTLPixelFormatBGRA8Unorm`
- `MTLPixelFormatBGRA8Unorm_sRGB`
- `MTLPixelFormatRGBA8Unorm`
- `MTLPixelFormatRGBA8Unorm_sRGB`
- `MTLPixelFormatRGB10A2Unorm`
- `MTLPixelFormatRGBA16Float`

Tradeoffs and limitations:

- The runtime can prove frame access at the GPU-resource level because Unity renders into textures allocated by the runtime.
- `METALXR_FRAME_DUMP_DIR` writes metadata for submitted projection frames, including swapchain ids, Metal texture pointers, image indices, formats, and rectangles.
- The runtime does not read pixels back to CPU memory yet.
- The runtime does not allocate IOSurface-backed textures yet, so there is no cross-process share handle or VideoToolbox encode path.
- Synchronization is minimal and only models the OpenXR acquire/wait/release call order. Real GPU fences must be added before encoding or streaming.

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

The probe uses `dlopen`, calls `xrNegotiateLoaderRuntimeInterface`, creates an instance with Unity's `XR_KHRX2_metal_enable` path, discovers the dummy HMD system, creates a session, consumes session-state events, creates a local reference space, creates one Metal swapchain per eye, and runs three deterministic frames through `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`, swapchain acquire/wait/release, and `xrEndFrame`.

The probe also sets `METALXR_FRAME_DUMP_DIR` and fails unless `frame_000001.txt` is written with projection frame metadata.

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

Issue #5 adds a standalone host encoder for synthetic stereo frames. The remaining integration work is connecting runtime-owned Metal textures to that encoder through a Metal blit into VideoToolbox-compatible pixel buffers or by changing swapchain allocation to IOSurface-backed textures that can be handed to the encoder with lower copy overhead.
