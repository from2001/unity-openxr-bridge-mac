# Release Checklist

This checklist is for producing a developer preview build of the Unity OpenXR Mac Bridge workflow.

## Preconditions

- Worktree is clean before release preparation starts.
- Quest is in Developer Mode and visible as `device` in `adb devices -l`.
- Unity version matches `TestProjects/UnityOpenXRSmoke/ProjectSettings/ProjectVersion.txt`.
- Android build support is installed for that Unity editor.
- Any license or attribution notes are reviewed before publishing public artifacts.

## Build Artifacts

Native runtime:

```sh
Scripts/build-metalxr-runtime.sh
```

Host tools:

```sh
Scripts/build-metalxr-host.sh
```

Shared protocol probe:

```sh
Scripts/build-metalxr-protocol.sh
```

Quest client APK:

```sh
Scripts/build-quest-client-apk.sh
```

macOS helper app:

```sh
xcodebuild -project MetalXR.xcodeproj -scheme MetalXR -configuration Debug -destination 'platform=macOS' build CODE_SIGNING_ALLOWED=NO
```

Expected local artifacts:

- `Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib`
- `Runtime/MetalXRHost/build/metalxr_host_streamer`
- `Runtime/MetalXRHost/build/metalxr_host_encoder`
- `Runtime/MetalXRProtocol/build/metalxr_protocol_loopback`
- `TestProjects/UnityOpenXRSmoke/Builds/MetalXRQuestClient.apk`
- `MetalXR.app` in Xcode DerivedData build products

## Smoke Tests

Native runtime:

```sh
Scripts/probe-metalxr-runtime.sh
```

Input, stale tracking/timing, and haptics:

```sh
Scripts/probe-metalxr-input-bridge.sh
```

Host encoder:

```sh
Scripts/probe-metalxr-host-encoder.sh
```

Frame transport, Unity-export fixture, and reconnect:

```sh
Scripts/probe-metalxr-frame-stream.sh
```

Protocol loopback:

```sh
Scripts/probe-metalxr-protocol.sh
```

Unity Quest client compile:

```sh
Scripts/probe-quest-client-unity.sh
```

End-to-end Quest smoke:

```sh
Scripts/run-metalxr-playmode-workflow.sh
METALXR_FRAME_EXPORT_MODE=readback Scripts/run-metalxr-playmode-workflow.sh
```

Quest screenshot check:

```sh
Scripts/probe-quest-client-screenshot.sh
```

## Diagnostics To Attach

- `Scripts/metalxr-status.sh` output.
- Runtime log from `METALXR_RUNTIME_LOG`.
- Streamer log from `METALXR_STREAMER_LOG` or the macOS app diagnostics export.
- Unity log from `~/Library/Logs/Unity/Editor.log` or workflow log.
- Quest logcat lines containing `MetalXRQuestClient`.
- Quest screenshot from `METALXR_SCREENSHOT_PATH`.

## Release Notes Template

```text
## Unity OpenXR Mac Bridge developer preview

### Included
- macOS OpenXR runtime skeleton for Unity Play Mode.
- Host H.264 streamer for Unity-exported stereo frames.
- Quest client APK for stream display and input/timing return path.
- macOS helper app for local workflow orchestration.

### Known limitations
- No production zero-copy frame handoff.
- No audio stream.
- TCP transport only.
- Limited OpenXR action/profile coverage.
- Not SteamVR/OpenComposite compatible.

### Required setup
- macOS 13+ with Metal.
- Unity 6 with Android build support.
- Quest Developer Mode and USB debugging.
```
