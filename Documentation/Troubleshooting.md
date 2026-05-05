# Troubleshooting

Use `Scripts/metalxr-status.sh` first. It prints adb devices, runtime manifests, built native tools, and installed Unity editors.

## Quest Does Not Appear In adb

Check:

```sh
Libs/adb-lib/adb devices -l
```

Expected state is `device`. If the device is `unauthorized`, put on the headset and accept the USB debugging prompt. If it is missing, reconnect USB, unlock the headset, and confirm Developer Mode is enabled in the Meta mobile app.

## Quest Client Is Not Installed

Build and install:

```sh
Scripts/build-quest-client-apk.sh
Scripts/install-run-quest-client.sh
```

The package name is `com.metalxr.questclient`.

## HMD Shows Black Or Very Dark Output

Capture a device screenshot:

```sh
Scripts/probe-quest-client-screenshot.sh
```

Then inspect:

```sh
Libs/adb-lib/adb logcat -d Unity:D '*:S' | grep MetalXRQuestClient
tail -n 120 "${TMPDIR:-/tmp}/metalxr_host_streamer.log"
tail -n 120 "${TMPDIR:-/tmp}/metalxr_runtime.log"
```

Common causes:

- Quest client is running but the host streamer is stopped.
- `adb reverse tcp:47000 tcp:47000` was lost after a USB reconnect.
- Unity is not in Play Mode or is not exporting complete left/right frame pairs.
- MediaCodec output is unavailable; the client should fall back to a compressed-payload preview.

## Streamer Cannot Receive A Quest Client

For USB:

```sh
Libs/adb-lib/adb reverse tcp:47000 tcp:47000
Scripts/run-metalxr-frame-stream.sh
```

`Scripts/run-metalxr-frame-stream.sh` refreshes adb reverse while running. Set `METALXR_ADB_REVERSE_REFRESH_SECONDS=0` only when you want to manage the reverse tunnel manually.

For Wi-Fi:

```sh
METALXR_TRANSPORT=wifi Scripts/run-metalxr-frame-stream.sh
```

Launch the Quest client with a host address reachable from the headset.

## Unity Opens With A Project Upgrade Prompt

The smoke project records its editor version in:

```text
TestProjects/UnityOpenXRSmoke/ProjectSettings/ProjectVersion.txt
```

Using the matching Unity version avoids unnecessary project reimport and setting churn. If you intentionally upgrade the project, commit the resulting Unity project setting and `.meta` changes after reviewing them.

## Unity Offers Safe Mode

Safe Mode means Unity detected compilation errors. Prefer `Quit`, then inspect:

```sh
tail -n 200 ~/Library/Logs/Unity/Editor.log
uloop compile --project-path TestProjects/UnityOpenXRSmoke --force-recompile true --wait-for-domain-reload true
```

Fix compile errors before entering Play Mode.

## Runtime Reports Stale Tracking Or Timing

Runtime logs include:

```text
tracking state stale
xrWaitFrame ... timing=stale
```

This means the Quest client or host streamer stopped updating input/timing files. Restart the Quest client or streamer. For USB, confirm adb reverse is active. During normal reconnects, the runtime keeps the HMD view pose valid so Unity continues rendering, but clears live tracking bits and controller/action state so Unity does not treat frozen samples as live input.

## VideoToolbox Encoder Fails

Run the encoder probe from a normal terminal session:

```sh
Scripts/probe-metalxr-host-encoder.sh
```

If `VTCompressionSessionCreate` fails, verify the Mac has hardware/software H.264 encoder support available and that the command is not running inside a restricted sandbox.

## Diagnostics Bundle

Use the macOS helper app's diagnostics export when reporting failures. It includes status output, Unity launch environment, runtime logs, streamer logs, and Quest logcat excerpts.
