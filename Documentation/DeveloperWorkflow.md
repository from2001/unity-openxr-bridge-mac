# Developer Workflow

This guide describes the repeatable Unity Editor Play Mode to Quest path for local development.

## 1. Check The Machine

```sh
Scripts/metalxr-status.sh
```

Confirm:

- `adb` is available.
- The Quest appears as `device` in `adb devices -l`.
- `Runtime/MetalXRRuntime/metalxr_runtime.json` exists.
- Unity Hub has the project Unity version installed.

## 2. Build Local Components

```sh
Scripts/build-metalxr-runtime.sh
Scripts/build-metalxr-host.sh
Scripts/build-metalxr-protocol.sh
```

Expected outputs:

- `Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib`
- `Runtime/MetalXRHost/build/metalxr_host_streamer`
- `Runtime/MetalXRHost/build/metalxr_host_encoder`
- `Runtime/MetalXRProtocol/build/metalxr_protocol_loopback`

Validate the runtime lifecycle and fixture export path:

```sh
Scripts/probe-metalxr-runtime.sh
```

Validate the experimental runtime IOSurface export path outside Unity:

```sh
METALXR_PROBE_FRAME_EXPORT_MODE=iosurface \
METALXR_PROBE_SWAPCHAIN_RESOURCE_MODE=iosurface \
METALXR_PROBE_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1 \
Scripts/probe-metalxr-runtime.sh
```

Validate the same IOSurface path without frame export files:

```sh
METALXR_PROBE_FRAME_EXPORT_DIR= \
METALXR_PROBE_FRAME_EXPORT_SOCKET="${TMPDIR:-/tmp}/metalxr-runtime-probe.sock" \
METALXR_PROBE_FRAME_EXPORT_MODE=iosurface \
METALXR_PROBE_SWAPCHAIN_RESOURCE_MODE=iosurface \
METALXR_PROBE_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1 \
Scripts/probe-metalxr-runtime.sh
```

## 3. Build And Install The Quest Client

```sh
Scripts/build-quest-client-apk.sh
Scripts/install-run-quest-client.sh
```

The default APK path is:

```text
TestProjects/UnityOpenXRSmoke/Builds/MetalXRQuestClient.apk
```

`install-run-quest-client.sh` installs the APK, applies `adb reverse tcp:47000 tcp:47000`, launches the Quest app, and prints recent Quest client logcat entries.

## 4. Run The Coordinated Smoke Workflow

```sh
Scripts/run-metalxr-playmode-workflow.sh
```

The workflow:

- launches the Quest client,
- launches the Unity smoke project with `XR_RUNTIME_JSON` pointing at the MetalXR runtime,
- enters Play Mode through uloop when available,
- waits for exported stereo frame pairs,
- starts the host streamer in `unity-export` mode,
- waits for Quest display logs,
- captures and analyzes an adb screenshot.

Default mode is deterministic fixture export:

```sh
METALXR_FRAME_EXPORT_MODE=fixture Scripts/run-metalxr-playmode-workflow.sh
```

Use readback mode when validating Unity-submitted texture data:

```sh
METALXR_FRAME_EXPORT_MODE=readback Scripts/run-metalxr-playmode-workflow.sh
```

`readback` mode defaults to `METALXR_WORKFLOW_QUALITY=balanced`, which uses a `1344x1408` runtime eye texture and a 40 Mbps H.264 target bitrate for Quest 3 smoke testing. Set `METALXR_WORKFLOW_QUALITY=debug` to use the older `640x360` path when you only need fast connection validation. `METALXR_WORKFLOW_QUALITY=native` uses `1832x1920` at 80 Mbps and is intended for isolated quality experiments because the current readback/file path is much heavier at that size.

Readback defaults to `METALXR_SWAPCHAIN_STORAGE_MODE=shared`. `managed` is retained only as a debugging override because it can stop Unity's OpenXR session on Apple Silicon.

To exercise restart cleanup and Quest reconnect behavior, run the repeat probe:

```sh
METALXR_WORKFLOW_REPEAT_COUNT=2 Scripts/probe-metalxr-playmode-workflow-repeat.sh
```

The repeat probe forces `METALXR_WORKFLOW_KEEP_RUNNING=0` and writes each run's Unity log, runtime log, streamer log, frame export directory, frame dump directory, and Quest screenshot into a separate output folder under `${TMPDIR:-/tmp}`.

## 5. Manual Play Mode Loop

Launch Unity with the runtime:

```sh
Scripts/launch-unity-openxr.sh TestProjects/UnityOpenXRSmoke
```

Enter Play Mode in Unity. Then start the stream:

```sh
METALXR_FRAME_SOURCE=unity-export \
METALXR_FRAME_EXPORT_DIR="${TMPDIR:-/tmp}/metalxr_unity_frames" \
Scripts/run-metalxr-frame-stream.sh
```

Manual `unity-export` streaming defaults to `METALXR_STREAM_QUALITY=balanced` so the encoder bitrate is high enough for Unity-rendered frames. For USB, the stream script binds to `127.0.0.1`, configures `adb reverse`, and refreshes the reverse tunnel every `METALXR_ADB_REVERSE_REFRESH_SECONDS` seconds while running.

## Useful Environment Variables

- `UNITY_APP` - explicit Unity.app path for batch builds.
- `UNITY_PROJECT_PATH` - Unity project path, defaulting to `TestProjects/UnityOpenXRSmoke`.
- `METALXR_RUNTIME_JSON` - explicit OpenXR runtime manifest.
- `METALXR_FRAME_EXPORT_MODE=fixture|readback`; `iosurface` is gated behind `METALXR_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1` and should only be used for isolated runtime validation.
- `METALXR_FRAME_EXPORT_DIR`
- `METALXR_FRAME_EXPORT_SOCKET` - optional Unix datagram socket path for frame metadata records. `METALXR_FRAME_EXPORT_MODE=iosurface` can use the socket without a frame export directory because the payload is carried by `ioSurfaceId`; `fixture` and `readback` still require the directory because they write payload files.
- `METALXR_FRAME_EXPORT_ACK_SOCKET` - optional Unix datagram socket path for IOSurface frame slot release acks. `Scripts/run-metalxr-playmode-workflow.sh` defaults this to `METALXR_FRAME_EXPORT_SOCKET.ack` when socket export is enabled.
- `METALXR_SWAPCHAIN_STORAGE_MODE=shared|managed|private`
- `METALXR_WORKFLOW_QUALITY=debug|balanced|native`
- `METALXR_STREAM_QUALITY=debug|balanced|native`
- `METALXR_STREAM_WIDTH`, `METALXR_STREAM_HEIGHT`, `METALXR_STREAM_FPS`
- `METALXR_STREAM_BITRATE`, `METALXR_STREAM_QUEUE_DEPTH`
- `METALXR_STREAM_RECONNECT_ATTEMPTS`
- `METALXR_TRACKING_STALE_TIMEOUT_MS`
- `METALXR_PREDICTION_OFFSET_MS`
- `METALXR_WORKFLOW_KEEP_RUNNING=1` to leave Unity and the streamer running after the workflow succeeds.
- `METALXR_WORKFLOW_UNITY_READY_SECONDS` to extend the Unity/uloop readiness wait during first imports or project upgrades.

## Expected Logs

- Runtime log: `${TMPDIR:-/tmp}/metalxr_runtime.log`
- Streamer log: `${TMPDIR:-/tmp}/metalxr_host_streamer.log`
- Quest screenshot: `${TMPDIR:-/tmp}/metalxr_quest_stream.png`
- Unity launch log: `${TMPDIR:-/tmp}/metalxr_unity_openxr.log`

The macOS helper app exports these logs plus status output and Quest logcat excerpts through its diagnostics button.
