# MetalXR Quest client shell

Issue #7 added the first Quest-side client surface inside the Unity OpenXR smoke project. Issue #8 extends it into a stream receiver: Unity's Android OpenXR loader owns the native headset session and stereo projection path, while the MetalXR scripts receive host VIDEO_FRAME packets, attempt H.264 decode with Android MediaCodec, and draw the resulting eye textures in front of the XR camera. Issue #9 adds Quest-to-host HMD/controller samples and host-to-Quest haptic commands.

## Project location

- Unity project: `TestProjects/UnityOpenXRSmoke`
- Runtime scripts: `Assets/MetalXRQuestClient`
- Build helper: `Assets/Editor/MetalXRQuestClientBuild.cs`
- Default package id: `com.metalxr.questclient`

At runtime, `MetalXRQuestClientBootstrap` creates a persistent client object. The display component creates an XR camera with generated left and right panels in front of it, keeps a diagnostic fallback texture active until stream frames arrive, converts MediaCodec YUV output into color RGBA textures when decode succeeds, and logs with the `MetalXRQuestClient` prefix so entries are easy to retrieve from adb logcat.

## Host handshake

The client attempts to connect to `127.0.0.1:47000` by default. This works with adb reverse:

```sh
adb reverse tcp:47000 tcp:47000
```

The packet format matches `Runtime/MetalXRProtocol/include/MetalXRProtocol/metalxr_protocol.h` for HELLO, HELLO_ACK, VIDEO_FRAME, POSE_SAMPLE, CONTROLLER_INPUT, and HAPTIC_COMMAND packets. The client advertises Quest client role, H.264, separate-eye stereo, pose input, controller input, haptics, and log-stream capability. A missing host is non-fatal; the client continues showing the diagnostic frame and periodically retries. If the stream disconnects, the background TCP thread reconnects without restarting Unity.

The host can be overridden with Unity command-line arguments when a launcher supports them:

```sh
--metalxr-host 127.0.0.1 --metalxr-port 47000
```

On Android, intent extras named `metalxr_host` and `metalxr_port` are also accepted.

## Frame stream

Build the host tools and run the stream probe locally:

```sh
Scripts/build-metalxr-host.sh
Scripts/probe-metalxr-frame-stream.sh
```

For a Quest device over USB:

```sh
Scripts/install-run-quest-client.sh
Scripts/run-metalxr-frame-stream.sh
```

For the full Unity Play Mode workflow:

```sh
Scripts/run-metalxr-playmode-workflow.sh
```

The workflow script installs and launches the Quest client, resets `adb reverse`, launches Unity with `METALXR_FRAME_EXPORT_DIR`, enters Play Mode through uloop when available, waits for exported stereo frame records, starts the host streamer in `unity-export` mode, waits for Quest display logs, and captures a screenshot smoke check.

By default, the workflow uses `METALXR_FRAME_EXPORT_MODE=fixture` to keep the end-to-end device smoke test deterministic with compact payloads. Set `METALXR_FRAME_EXPORT_MODE=readback` when validating CPU readback of Unity-submitted projection textures. Readback defaults to `METALXR_SWAPCHAIN_STORAGE_MODE=shared`; `managed` remains available as a debugging override but currently stops Unity's OpenXR session on Apple Silicon.

`Scripts/run-metalxr-frame-stream.sh` configures `adb reverse` in USB mode and then starts `Runtime/MetalXRHost/build/metalxr_host_streamer`. Useful stream controls are exposed as environment variables:

- `METALXR_STREAM_WIDTH` and `METALXR_STREAM_HEIGHT`
- `METALXR_STREAM_FPS`
- `METALXR_STREAM_BITRATE`
- `METALXR_STREAM_FRAMES`
- `METALXR_STREAM_QUEUE_DEPTH`
- `METALXR_STREAM_RECONNECT_ATTEMPTS`
- `METALXR_FRAME_SOURCE=synthetic|unity-export`
- `METALXR_FRAME_EXPORT_DIR`
- `METALXR_SWAPCHAIN_STORAGE_MODE=shared|managed|private`
- `METALXR_PREDICTION_OFFSET_MS`
- `METALXR_CLOCK_SYNC_INTERVAL_MS`
- `METALXR_TRANSPORT=usb|wifi`
- `METALXR_ADB_REVERSE_REFRESH_SECONDS`
- `METALXR_TRACKING_STATE_PATH`
- `METALXR_HAPTIC_COMMAND_PATH`
- `METALXR_TIMING_STATE_PATH`
- `METALXR_TRACKING_STALE_TIMEOUT_MS`

On Quest, the client reads VIDEO_FRAME packets on a background thread, queues encoded access units, and processes decode/display work on the Unity main thread. Android builds attempt MediaCodec H.264 decode first, read `Image.getPlanes()` YUV_420_888 output, convert it to RGBA, and upload the color result into the per-eye Unity textures. If MediaCodec is unavailable, output images are unavailable, or decoded color samples are near-black, the client displays a compressed-payload preview so transport and frame pacing remain visible during development. Display logs include decoder mode, output format, frame id, eye, host encoder latency, local client receive-to-display timing, decode time, submit time, and queue depth.

To make HMD/casting black-frame regressions visible during device smoke tests, capture and analyze a Quest screenshot:

```sh
Scripts/probe-quest-client-screenshot.sh
```

The script uses `adb exec-out screencap -p`, saves the PNG to `METALXR_SCREENSHOT_PATH` or `/tmp/metalxr_quest_stream.png`, and fails when sampled pixels are too dark or have too little color variation.

## Timing

The host sends `METALXR_PACKET_TIMING_SAMPLE` clock-sync probes and the Quest client replies immediately from the stream thread. After a frame is decoded and submitted to the eye texture, the Quest client sends a frame timing sample with receive, decode, display, and queue-depth timestamps.

The host logs latency JSON records with encode, network, decode, compositor-submit, total latency, prediction error, queue depth, and measured display period. It also writes `METALXR_TIMING_STATE_PATH`; the native runtime reads that file so `xrWaitFrame` predictions can follow measured Quest display timing instead of a fixed local guess.

## Tracking, Controllers, And Haptics

The Quest client samples the center-eye HMD, left controller, and right controller through Unity's XR input devices on the Unity main thread. It sends:

- `METALXR_PACKET_POSE_SAMPLE` for HMD position, orientation, and tracking flags.
- `METALXR_PACKET_CONTROLLER_INPUT` for controller buttons, trigger, grip, thumbstick, tracking flags, and aim/grip poses.

The macOS host streamer drains those packets on the stream socket and atomically rewrites `METALXR_TRACKING_STATE_PATH`. The native runtime consumes that file for `xrLocateViews`, action-space locations, and action-state reads. If the state file is older than `METALXR_TRACKING_STALE_TIMEOUT_MS` (default 1000), the runtime keeps the last pose for continuity but clears OpenXR tracking flags, button states, triggers, grips, and thumbsticks so Unity sees inactive/stale input instead of frozen valid tracking.

Haptics flow in the opposite direction. The runtime writes the latest command to `METALXR_HAPTIC_COMMAND_PATH` from `xrApplyHapticFeedback`; the host streamer sends it as `METALXR_PACKET_HAPTIC_COMMAND`; the Quest client applies it with Unity XR `SendHapticImpulse` on the main thread.

This is a development bridge for USB smoke tests. It does not yet provide datagram delivery or production reconnect policy.

## Build

Build the APK with:

```sh
Scripts/build-quest-client-apk.sh
```

The default output is `TestProjects/UnityOpenXRSmoke/Builds/MetalXRQuestClient.apk`. The build helper configures:

- Android application id `com.metalxr.questclient`
- IL2CPP
- ARM64 only
- Android internet permission
- Android XR Management settings with the Unity OpenXR loader assigned
- Meta Quest OpenXR support, hand tracking manifest entries, and the Hand Interaction Profile so adb launch does not require awake controllers

## Install, launch, and logs

With a Quest connected over adb and authorized:

```sh
Scripts/install-run-quest-client.sh
```

The script installs the APK, configures `adb reverse tcp:47000 tcp:47000`, launches the package through the Quest VR category, and prints recent `MetalXRQuestClient` Unity logs. To keep following logs:

```sh
METALXR_LOGCAT_FOLLOW=1 Scripts/install-run-quest-client.sh
```

## Local compile probe

When Unity is already open on the smoke project:

```sh
Scripts/probe-quest-client-unity.sh
```

This uses `uloop compile` to verify the C# client and build helper without requiring a headset.
