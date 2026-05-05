# MetalXR | OpenXR experiments on macOS

MetalXR is a work-in-progress macOS helper for OpenXR development. The original repository described a full OpenXR/OpenComposite runtime for macOS, but the checked-in code only contained a SwiftUI app and a bundled Android platform-tools/adb folder. This fork now makes the existing app runnable as a Quest APK installer and adds scripts for launching Unity with a macOS OpenXR runtime manifest.

## Current status

Working:

- Detect an authorized Quest/Android headset through the bundled `adb`.
- Install a local APK onto the headset from the macOS app or from `Scripts/install-quest-apk.sh`.
- Launch Unity with `XR_RUNTIME_JSON` set to an installed macOS OpenXR runtime, without changing system-wide OpenXR runtime registration.
- Use Meta XR Simulator as the default macOS OpenXR runtime when it is installed at `/Applications/MetaXRSimulator.app`.
- Build and probe a native MetalXR OpenXR runtime that can create an instance, report a dummy stereo HMD system, create a session, emit lifecycle events, create reference spaces, and run a deterministic frame loop.
- Create runtime-owned Metal swapchain textures for Unity OpenXR Play Mode, accept stereo projection layers, and dump per-frame metadata proving which textures were submitted.
- Build and probe a macOS VideoToolbox host encoder that converts synthetic stereo frames into H.264 elementary streams with per-frame latency/drop metadata.
- Build and probe shared host/client protocol packet definitions with loopback handshake, heartbeat, and version-mismatch handling.
- Build a Quest/Android Unity OpenXR client shell that displays generated stereo diagnostic frames in-headset and attempts the shared HELLO handshake over an adb-reversed host endpoint.

Not implemented yet:

- CPU pixel readback, IOSurface export, or VideoToolbox encoding of Unity-submitted Metal textures.
- A Quest PCVR media transport layer for video, tracking, input, audio, and timing.
- A native MetalXR streamer that sends encoded Unity frames to the Quest client.
- SteamVR/OpenComposite compatibility on macOS.

That means this repository can currently make Unity's OpenXR loader run on macOS against an existing runtime such as Meta XR Simulator, but it is not yet the macOS equivalent of Quest Link/Air Link. [Unity's Meta Quest Link documentation](https://docs.unity.cn/Packages/com.unity.xr.meta-openxr%402.1/manual/get-started/link.html) states that Meta Quest Link is Windows-only, so a real Mac version requires implementing both a host OpenXR runtime and a headset streaming client.

## Quick start for Unity on macOS

Check local status:

```sh
Scripts/metalxr-status.sh
```

Build and probe the native MetalXR runtime:

```sh
Scripts/build-metalxr-runtime.sh
Scripts/probe-metalxr-runtime.sh
```

Build and probe the macOS host encoder:

```sh
Scripts/build-metalxr-host.sh
Scripts/probe-metalxr-host-encoder.sh
```

Build and probe the shared protocol:

```sh
Scripts/build-metalxr-protocol.sh
Scripts/probe-metalxr-protocol.sh
```

Build and locally compile-probe the Quest client shell:

```sh
Scripts/build-quest-client-apk.sh
Scripts/probe-quest-client-unity.sh
```

Launch Unity with the default runtime. If Meta XR Simulator is installed, it is selected automatically:

```sh
Scripts/launch-unity-openxr.sh /path/to/UnityProject
```

If the MetalXR runtime has been built, the launch script prefers `Runtime/MetalXRRuntime/metalxr_runtime.json`. Otherwise it falls back to Meta XR Simulator when present.

Use another OpenXR runtime manifest:

```sh
METALXR_RUNTIME_JSON=/path/to/runtime.json Scripts/launch-unity-openxr.sh /path/to/UnityProject
```

Install a local Quest APK:

```sh
Scripts/install-quest-apk.sh /path/to/app.apk
```

Install and launch the MetalXR Quest client APK:

```sh
Scripts/install-run-quest-client.sh
```

The headset must have Developer Mode enabled, USB debugging accepted in-headset, and must appear as `device` in `adb devices -l`.

## Unity project setup

Use Unity's OpenXR package for Standalone/macOS Play Mode and Android/Quest builds. For Meta Quest content, keep the Android build path as the production target and use the macOS OpenXR path for editor simulation or future native runtime work.

For Meta XR Simulator:

1. Install Meta XR Simulator.
2. Run `Scripts/launch-unity-openxr.sh`.
3. In Unity, enable OpenXR for the Standalone target.
4. Enter Play Mode after the simulator has started.

The script sets `XR_RUNTIME_JSON` only for the Unity process it launches. It follows the [OpenXR loader override mechanism](https://registry.khronos.org/OpenXR/specs/1.1/loader.html#overriding-the-default-runtime-usage) and does not overwrite `/usr/local/share/openxr/1/active_runtime.json`.

When the MetalXR runtime is selected, the script also sets:

- `METALXR_RUNTIME_LOG` for runtime lifecycle and swapchain logs.
- `METALXR_FRAME_DUMP_DIR` for per-frame metadata files written from `xrEndFrame`.

## How does it work?

The macOS app is a SwiftUI wrapper around bundled adb platform-tools. The scripts provide a lower-friction development path:

- `Scripts/metalxr-status.sh` prints adb devices, OpenXR runtime manifests, and installed Unity editors.
- `Scripts/install-quest-apk.sh` installs an APK through adb with device-state checks.
- `Scripts/launch-unity-openxr.sh` launches Unity with `XR_RUNTIME_JSON` pointing at a runtime manifest.
- `Scripts/build-metalxr-runtime.sh` builds the native runtime.
- `Scripts/probe-metalxr-runtime.sh` verifies OpenXR loader/runtime negotiation, the dummy HMD lifecycle, Metal swapchain creation, and projection frame metadata dumps.
- `Scripts/build-metalxr-host.sh` builds the macOS host utilities.
- `Scripts/probe-metalxr-host-encoder.sh` verifies continuous VideoToolbox H.264 encoding from a synthetic stereo frame stream.
- `Scripts/build-metalxr-protocol.sh` builds shared host/client protocol utilities.
- `Scripts/probe-metalxr-protocol.sh` verifies loopback handshake, heartbeat, and version-mismatch handling.
- `Scripts/build-quest-client-apk.sh` builds the Unity OpenXR smoke project as a Quest APK.
- `Scripts/install-run-quest-client.sh` installs, launches, configures adb reverse, and prints client logcat entries.
- `Scripts/probe-quest-client-unity.sh` compiles the Unity Quest client scripts through uloop.

The next major engineering step is connecting runtime-owned Metal textures to the host encoder and media transport, then decoding and displaying those frames on the Quest client. After that, tracking, input, haptics, and timing control need to be integrated.

## How can I install it?

There is no release artifact for this fork yet. Build the macOS app with Xcode:

```sh
xcodebuild -project MetalXR.xcodeproj -scheme MetalXR -configuration Debug -destination 'platform=macOS' build CODE_SIGNING_ALLOWED=NO
```

## What are the requirements?  
As of now, we only support standalone* Android headsets (Meta Quest, Pico Neo, etc) because of limitations with macOS USB and DisplayPort handling.

As a minimum, you'll want:
- A machine running **macOS Ventura 13.0 or later**
- A graphics card capable of the full **Metal 2 API**.
- 8GB of system memory
- 256GB of SSD storage
- A standalone VR headset
  - Our tests will be conducted with the Meta Quest 2 and Pico 4
  - PCVR-only headset support is being researched
  - Free to create a PR if you have added support for PCVR-only or a new headset!

We recommend:
- A machine running the latest macOS release
- A **dedicated** graphics card capable of the full **Metal 3 API**
- 4GB of video memory
- 16GB of system memory
- 256GB of SSD storage
- A standalone VR headset

If you don't know whether or not your GPU supports Metal 2 or Metal 3, [you can check here.](https://crystall1ne.dev/2023/05/03/metal-1-2-or-3/)

## Credits and more info!
This project has been created and is maintained by the following:
* [CADIndie](https://github.com/CADIndie) | Project Manager/Researcher
* [crystall1nedev](https://github.com/crystall1nedev) | Lead Swift/macOS Developer
* [TheJudge156](https://github.com/thejudge156) | Lead OXR and OC Developer

(more coming soon!)
