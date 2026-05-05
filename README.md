# Unity OpenXR Mac Bridge

Unity OpenXR Mac Bridge is an experimental macOS bridge for running Unity Editor Play Mode on an OpenXR headset.

## Current status

Working:

- Detect an authorized Quest/Android headset through the bundled `adb`.
- Install a local APK onto the headset from the macOS app or from `Scripts/install-quest-apk.sh`.
- Use the macOS SwiftUI app as a developer dashboard for Quest connection, Quest client install state, runtime build state, Unity launch, host streaming, and diagnostics export.
- Launch Unity with `XR_RUNTIME_JSON` set to an installed macOS OpenXR runtime, without changing system-wide OpenXR runtime registration.
- Use Meta XR Simulator as the default macOS OpenXR runtime when it is installed at `/Applications/MetaXRSimulator.app`.
- Build and probe a native MetalXR OpenXR runtime that can create an instance, report a dummy stereo HMD system, create a session, emit lifecycle events, create reference spaces, and run a deterministic frame loop.
- Create runtime-owned Metal swapchain textures for Unity OpenXR Play Mode, accept stereo projection layers, and dump per-frame metadata proving which textures were submitted.
- Export submitted projection views as per-eye frame records with host-readable BGRA/raw payload files when `METALXR_FRAME_EXPORT_DIR` is set.
- Build and probe a macOS VideoToolbox host encoder that converts synthetic stereo frames or exported Unity BGRA eye frames into H.264 elementary streams with per-frame latency/drop metadata.
- Build and probe shared host/client protocol packet definitions with loopback handshake, heartbeat, and version-mismatch handling.
- Build a Quest/Android Unity OpenXR client shell that displays generated stereo diagnostic frames in-headset and attempts the shared HELLO handshake over an adb-reversed host endpoint.
- Stream synthetic or Unity-exported macOS VideoToolbox H.264 stereo frames over the shared TCP protocol to the Quest client, with MediaCodec decode attempted on-device and receive-to-display timing logged from Unity.
- Bridge Quest HMD pose and controller samples back to the macOS host, expose them to the native runtime through a development state file, and map them into minimal OpenXR view, action-state, action-space, and haptic APIs.
- Exchange timing samples for development clock sync, log encode/network/decode/submit/total latency breakdowns, and feed measured Quest display timing into runtime `xrWaitFrame` predictions.

Not implemented yet:

- Production IOSurface export, zero-copy frame handoff, or direct VideoToolbox encoding of Unity-submitted Metal textures.
- A production Quest PCVR transport layer for audio, loss recovery, adaptive timing, and low-latency datagrams.
- Production-grade OpenXR action binding/profile coverage, true separate aim/grip poses on every device path, and predictive input timing.
- A fully wired one-click Unity Play Mode to Quest workflow.
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
Scripts/probe-metalxr-input-bridge.sh
```

Build and probe the macOS host encoder:

```sh
Scripts/build-metalxr-host.sh
Scripts/probe-metalxr-host-encoder.sh
Scripts/probe-metalxr-frame-stream.sh
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

Run the host frame streamer over USB after the Quest client is launched:

```sh
Scripts/run-metalxr-frame-stream.sh
```

Run the host streamer from Unity-exported frames after launching Unity with `METALXR_FRAME_EXPORT_DIR`:

```sh
METALXR_FRAME_SOURCE=unity-export \
METALXR_FRAME_EXPORT_DIR=/tmp/metalxr_frame_export \
Scripts/run-metalxr-frame-stream.sh
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
- `METALXR_FRAME_EXPORT_DIR` for per-eye frame export records and payload files written from submitted projection views.
- `METALXR_FRAME_EXPORT_MODE=readback|fixture` for CPU readback or deterministic probe payloads.
- `METALXR_TRACKING_STATE_PATH` for HMD/controller state consumed by view and action APIs.
- `METALXR_HAPTIC_COMMAND_PATH` for haptic commands emitted by `xrApplyHapticFeedback`.
- `METALXR_TIMING_STATE_PATH` for measured Quest timing consumed by `xrWaitFrame`.
- `METALXR_VIEW_WIDTH`, `METALXR_VIEW_HEIGHT`, `METALXR_REFRESH_RATE`, and `METALXR_PREDICTION_OFFSET_MS` for runtime tuning.

## How does it work?

The macOS app is a SwiftUI dashboard around bundled adb platform-tools and the repository scripts. It can install the built Quest client APK, launch Unity through the MetalXR runtime script, start or stop the USB host streamer, and export a diagnostics folder containing status output, launch environment, runtime logs, streamer logs, and Quest logcat excerpts. The scripts remain available for terminal-first workflows:

- `Scripts/metalxr-status.sh` prints adb devices, OpenXR runtime manifests, and installed Unity editors.
- `Scripts/install-quest-apk.sh` installs an APK through adb with device-state checks.
- `Scripts/launch-unity-openxr.sh` launches Unity with `XR_RUNTIME_JSON` pointing at a runtime manifest.
- `Scripts/build-metalxr-runtime.sh` builds the native runtime.
- `Scripts/probe-metalxr-runtime.sh` verifies OpenXR loader/runtime negotiation, the dummy HMD lifecycle, Metal swapchain creation, and projection frame metadata dumps.
- `Scripts/probe-metalxr-input-bridge.sh` verifies runtime-side HMD pose, controller action states, action spaces, and haptic command output.
- `Scripts/build-metalxr-host.sh` builds the macOS host utilities.
- `Scripts/probe-metalxr-host-encoder.sh` verifies continuous VideoToolbox H.264 encoding from a synthetic stereo frame stream.
- `Scripts/probe-metalxr-frame-stream.sh` verifies TCP HELLO/HELLO_ACK negotiation plus streamed H.264 VIDEO_FRAME packets for both synthetic frames and a Unity-export fixture.
- `Scripts/build-metalxr-protocol.sh` builds shared host/client protocol utilities.
- `Scripts/probe-metalxr-protocol.sh` verifies loopback handshake, heartbeat, and version-mismatch handling.
- `Scripts/build-quest-client-apk.sh` builds the Unity OpenXR smoke project as a Quest APK.
- `Scripts/install-run-quest-client.sh` installs, launches, configures adb reverse, and prints client logcat entries.
- `Scripts/run-metalxr-frame-stream.sh` runs the host streamer for USB adb reverse or Wi-Fi transport tests, using `METALXR_FRAME_SOURCE=synthetic|unity-export`.
- `Scripts/probe-quest-client-unity.sh` compiles the Unity Quest client scripts through uloop.

The next major engineering step is rendering the decoded color video on Quest without diagnostic fallback, then wiring the full Unity Play Mode workflow and improving adaptive frame pacing, input prediction, loss recovery, and timing control.

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
