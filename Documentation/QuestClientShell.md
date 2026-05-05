# MetalXR Quest client shell

Issue #7 adds the first Quest-side client surface inside the Unity OpenXR smoke project. It is intentionally a shell: Unity's Android OpenXR loader owns the native headset session and stereo projection path, while the MetalXR scripts draw a diagnostic stereo frame and attempt the shared protocol HELLO handshake with a host endpoint.

## Project location

- Unity project: `TestProjects/UnityOpenXRSmoke`
- Runtime scripts: `Assets/MetalXRQuestClient`
- Build helper: `Assets/Editor/MetalXRQuestClientBuild.cs`
- Default package id: `com.metalxr.questclient`

At runtime, `MetalXRQuestClientBootstrap` creates a persistent client object. The display component creates an XR camera with generated left and right diagnostic frame panels in front of it, and logs with the `MetalXRQuestClient` prefix so entries are easy to retrieve from adb logcat.

## Host handshake

The client attempts to connect to `127.0.0.1:47000` by default. This works with adb reverse:

```sh
adb reverse tcp:47000 tcp:47000
```

The packet format matches `Runtime/MetalXRProtocol/include/MetalXRProtocol/metalxr_protocol.h` for the HELLO header and payload. The shell advertises Quest client role, H.264, separate-eye stereo, and log-stream capability. A missing host is non-fatal; the client continues showing the diagnostic frame and periodically retries.

The host can be overridden with Unity command-line arguments when a launcher supports them:

```sh
--metalxr-host 127.0.0.1 --metalxr-port 47000
```

On Android, intent extras named `metalxr_host` and `metalxr_port` are also accepted.

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
