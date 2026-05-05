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

Readback defaults to `METALXR_SWAPCHAIN_STORAGE_MODE=shared`. `managed` is retained only as a debugging override because it can stop Unity's OpenXR session on Apple Silicon.

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

For USB, the stream script binds to `127.0.0.1`, configures `adb reverse`, and refreshes the reverse tunnel every `METALXR_ADB_REVERSE_REFRESH_SECONDS` seconds while running.

## Useful Environment Variables

- `UNITY_APP` - explicit Unity.app path for batch builds.
- `UNITY_PROJECT_PATH` - Unity project path, defaulting to `TestProjects/UnityOpenXRSmoke`.
- `METALXR_RUNTIME_JSON` - explicit OpenXR runtime manifest.
- `METALXR_FRAME_EXPORT_MODE=fixture|readback`
- `METALXR_FRAME_EXPORT_DIR`
- `METALXR_SWAPCHAIN_STORAGE_MODE=shared|managed|private`
- `METALXR_STREAM_WIDTH`, `METALXR_STREAM_HEIGHT`, `METALXR_STREAM_FPS`
- `METALXR_STREAM_BITRATE`, `METALXR_STREAM_QUEUE_DEPTH`
- `METALXR_STREAM_RECONNECT_ATTEMPTS`
- `METALXR_TRACKING_STALE_TIMEOUT_MS`
- `METALXR_PREDICTION_OFFSET_MS`
- `METALXR_WORKFLOW_KEEP_RUNNING=1` to leave Unity and the streamer running after the workflow succeeds.

## Expected Logs

- Runtime log: `${TMPDIR:-/tmp}/metalxr_runtime.log`
- Streamer log: `${TMPDIR:-/tmp}/metalxr_host_streamer.log`
- Quest screenshot: `${TMPDIR:-/tmp}/metalxr_quest_stream.png`
- Unity launch log: `${TMPDIR:-/tmp}/metalxr_unity_openxr.log`

The macOS helper app exports these logs plus status output and Quest logcat excerpts through its diagnostics button.
