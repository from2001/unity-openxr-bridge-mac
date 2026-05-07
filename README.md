# Unity OpenXR Mac Bridge

Unity OpenXR Mac Bridge is an experimental macOS bridge for running Unity Editor Play Mode on an OpenXR headset.

The current target is a Mac Unity Editor session rendered through a local OpenXR runtime, encoded on macOS, streamed to a Quest client, and driven by Quest headset/controller input. It is a developer workflow for Play Mode iteration, not a production replacement for Quest Link, Air Link, SteamVR, or OpenComposite.

## Current Stage

Working:

- macOS SwiftUI developer dashboard for Quest detection, APK install, Unity launch, stream start/stop, and diagnostics export.
- Native macOS OpenXR runtime skeleton with lifecycle, session events, reference spaces, Metal swapchains, stereo projection submission, frame metadata, and frame export.
- VideoToolbox host encoder and TCP streamer for synthetic frames or Unity-exported stereo eye frames, using IOSurface/Metal-compatible `CVPixelBufferPool` encoder slots.
- Optional Unix-datagram frame metadata channel between the runtime export path and host streamer, with file export retained as the debug capture path.
- Unity Quest client APK that connects over USB `adb reverse`, advertises a device profile during HELLO, decodes H.264 frames with MediaCodec when available, displays the stream in headset, and sends timing/input samples back to the host.
- Development tracking/action/haptic bridge for HMD pose, controller buttons, trigger, grip, thumbstick, aim/grip poses, and haptic feedback.
- Runtime frame timing fed by Quest display samples, OpenXR projection metadata carried with each streamed eye frame, reconnect handling for common Quest client restarts, stale tracking/timing diagnostics, and smoke probes for the full loop.

Not MVP-ready yet:

- Production zero-copy runtime-to-host IOSurface frame handoff. Frame transfer currently still uses CPU readback and file-based export before the host copies pixels into encoder slots. Runtime-emitted IOSurface ids are available only behind an explicit experimental guard.
- Production runtime-host IPC/ring ownership beyond the current single-slot shared-state bridge.
- Production Quest presentation. The Quest APK is currently a diagnostic Unity OpenXR client that displays streamed eye textures through an in-headset debug presentation path.
- Pose/FOV-aware Quest presentation, rotational reprojection, or compositor-layer style presentation.
- Datagram transport, packet loss recovery, audio, foveation, or automatic bitrate adaptation.
- Full OpenXR interaction profile/action binding coverage.
- SteamVR/OpenComposite compatibility.
- Signed/notarized release packaging.

## Architecture Direction

The intended long-term model is still OpenXR-runtime-first:

```text
Unity Editor
  -> MetalXR macOS OpenXR runtime
  -> MetalXR host service
  -> Quest/OpenXR headset client
```

Unity camera data should not become a separate bridge API. The runtime should carry the OpenXR projection-layer metadata that Unity actually submitted: predicted display time, per-eye pose, FOV, image rect, swapchain identity, and reference-space context. That keeps the bridge aligned with OpenXR instead of adding Unity-specific camera coupling.

The current implementation deliberately keeps several development-only boundaries because they are easy to probe and debug:

- Unity-submitted Metal frames are exported through CPU readback and files before the host copies them into IOSurface-backed encoder pixel buffers.
- The host applies the Quest HELLO device profile for stream compatibility, but the runtime still uses a fixed development view configuration until the profile bridge moves into runtime-host IPC.
- Runtime-host tracking, timing, and haptics now have a POSIX shared-state bridge; debug text files remain as fallback and diagnostics.
- The Quest client uses a Unity diagnostic presentation path and keeps a CPU MediaCodec Image-plane fallback.

The next production-oriented milestones are to connect runtime-owned textures to host encoder slots through shared GPU resources, move the shared-state bridge to richer IPC/ring ownership, and add a Quest presentation path that is aware of frame pose/FOV rather than just showing diagnostic panels.

## Repository Layout

- `MetalXR/` - macOS SwiftUI helper app.
- `Runtime/MetalXRRuntime/` - native macOS OpenXR runtime.
- `Runtime/MetalXRHost/` - host encoder and stream server.
- `Runtime/MetalXRProtocol/` - shared host/client packet protocol.
- `TestProjects/UnityOpenXRSmoke/` - Unity project used for the Quest client APK and smoke workflow.
- `Scripts/` - build, launch, install, probe, and workflow scripts.
- `Documentation/` - architecture notes, workflow guide, troubleshooting, and release checklist.

## Requirements

- macOS 13 or newer with Metal support.
- Unity 6 matching `TestProjects/UnityOpenXRSmoke/ProjectSettings/ProjectVersion.txt` for the included smoke project.
- Android build support installed in Unity when building the Quest APK.
- Meta Quest with Developer Mode enabled and USB debugging accepted.
- `adb` from the bundled `Libs/adb-lib/adb` or from `PATH`.
- Optional: `uloop` for automated Unity compile/Play Mode smoke runs.

## Quick Start

Check local state:

```sh
Scripts/metalxr-status.sh
```

Build the native runtime, host tools, and protocol probe:

```sh
Scripts/build-metalxr-runtime.sh
Scripts/build-metalxr-host.sh
Scripts/build-metalxr-protocol.sh
```

Build the Quest client APK:

```sh
Scripts/build-quest-client-apk.sh
```

Install and launch the Quest client:

```sh
Scripts/install-run-quest-client.sh
```

Run the coordinated Unity Play Mode smoke workflow:

```sh
Scripts/run-metalxr-playmode-workflow.sh
```

By default the workflow uses deterministic fixture frames so the launcher, runtime, streamer, Quest client, log, and screenshot path can be validated quickly. To validate Unity texture readback, run:

```sh
METALXR_FRAME_EXPORT_MODE=readback Scripts/run-metalxr-playmode-workflow.sh
```

`readback` mode defaults to the `debug` quality preset for smoke reliability because the current CPU Image-plane decode path on Quest can fall behind at higher resolutions. Use `METALXR_WORKFLOW_QUALITY=balanced` for `1344x1408` per eye at 40 Mbps, `native` for isolated quality experiments, or override `METALXR_VIEW_WIDTH`, `METALXR_VIEW_HEIGHT`, and `METALXR_STREAM_BITRATE` explicitly.

To validate the guarded IOSurface/socket transport path in the coordinated workflow, run:

```sh
METALXR_WORKFLOW_FRAME_TRANSPORT=iosurface Scripts/run-metalxr-playmode-workflow.sh
```

This mode uses socket-only IOSurface frame records with release acks and keeps readback/file export available as the debug fallback.

The Quest client now starts in `projection` presentation mode by default. It uses the per-eye projection metadata carried by each VIDEO_FRAME packet to size the in-headset eye panels and applies a default Quest render scale of 1.2 so the diagnostic Unity presentation does not render below headset resolution. Use `METALXR_QUEST_PRESENTATION_MODE=diagnostic` to force the old fixed debug panel layout, or `METALXR_QUEST_RENDER_SCALE=1.0` to lower the Quest app render scale for stability experiments.

For restart and reconnect smoke testing, run the workflow repeat probe:

```sh
METALXR_WORKFLOW_REPEAT_COUNT=2 Scripts/probe-metalxr-playmode-workflow-repeat.sh
```

The coordinated workflow prints latency metrics from the streamer log and fails when the latest exported frame age or Quest queue depth exceeds the configured smoke thresholds.

If Unity left scene recovery backups after a crash or forced quit, the workflow archives `Temp/__Backupscenes` and `Assets/_Recovery` before launch so the editor does not block on the recovery prompt. `Scripts/launch-unity-openxr.sh` performs the same preflight for direct launches. Set `METALXR_WORKFLOW_ARCHIVE_SCENE_RECOVERY=0` for the workflow or `METALXR_ARCHIVE_SCENE_RECOVERY=0` for the direct launcher to keep those folders in place for manual recovery.

To compare the current readback export with the guarded IOSurface/socket metadata path:

```sh
Scripts/probe-metalxr-frame-export-modes.sh
```

For a manual stream after Unity is already exporting frames:

```sh
METALXR_FRAME_SOURCE=unity-export \
METALXR_FRAME_EXPORT_DIR=/tmp/metalxr_unity_frames \
Scripts/run-metalxr-frame-stream.sh
```

Manual `unity-export` streaming also defaults to the `balanced` stream preset. Use `METALXR_STREAM_QUALITY=debug` when validating only connection and packet flow.

## Documentation

- [Developer Workflow](Documentation/DeveloperWorkflow.md)
- [Troubleshooting](Documentation/Troubleshooting.md)
- [Release Checklist](Documentation/ReleaseChecklist.md)
- [OpenXR Runtime Notes](Documentation/OpenXRRuntimeSkeleton.md)
- [Host Encoder Notes](Documentation/MetalXRHostEncoder.md)
- [Protocol Notes](Documentation/MetalXRProtocol.md)
- [Quest Client Notes](Documentation/QuestClientShell.md)

## Verification Commands

```sh
Scripts/probe-metalxr-runtime.sh
Scripts/probe-metalxr-input-bridge.sh
Scripts/probe-metalxr-host-encoder.sh
Scripts/probe-metalxr-frame-stream.sh
Scripts/probe-metalxr-protocol.sh
Scripts/probe-quest-client-unity.sh
Scripts/probe-quest-client-screenshot.sh
```

Some probes require access to macOS Metal, VideoToolbox, local TCP listen ports, Unity, or an attached Quest.

## Build Artifacts

- Runtime dylib: `Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib`
- Runtime manifest: `Runtime/MetalXRRuntime/metalxr_runtime.json`
- Host streamer: `Runtime/MetalXRHost/build/metalxr_host_streamer`
- Host encoder probe binary: `Runtime/MetalXRHost/build/metalxr_host_encoder`
- Quest APK: `TestProjects/UnityOpenXRSmoke/Builds/MetalXRQuestClient.apk`
- macOS helper app: built by Xcode from `MetalXR.xcodeproj`

Build the macOS helper app:

```sh
xcodebuild -project MetalXR.xcodeproj -scheme MetalXR -configuration Debug -destination 'platform=macOS' build CODE_SIGNING_ALLOWED=NO
```

## Credits

This work builds on the idea and early macOS launcher work from [CADIndie/MetalXR](https://github.com/CADIndie/MetalXR). Appreciation goes to CADIndie, crystall1nedev, and TheJudge156 for starting the project direction. This fork is focused on the Unity Editor Play Mode to Quest workflow described above.
