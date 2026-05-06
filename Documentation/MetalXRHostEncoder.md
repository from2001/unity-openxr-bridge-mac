# MetalXR Host Encoder

This document tracks the macOS host encoder added for issue #5.

## Purpose

The host encoder is an independent macOS command-line component that converts frame sources into a low-latency H.264 stream with VideoToolbox. It supports synthetic stereo BGRA frames for diagnostics and file-based Unity exports written by the OpenXR runtime for development integration.

The implementation does not call Unity APIs. Frame generation, IOSurface-backed pixel buffer pool ownership, and VideoToolbox encoding all run in the host process.

## Build

```sh
Scripts/build-metalxr-host.sh
```

The binary is built at:

```text
Runtime/MetalXRHost/build/metalxr_host_encoder
Runtime/MetalXRHost/build/metalxr_host_streamer
Runtime/MetalXRHost/build/metalxr_iosurface_export_fixture
```

The build script uses CMake when available and falls back to direct `clang` compilation against:

- CoreFoundation
- CoreMedia
- CoreVideo
- IOSurface
- VideoToolbox

## Probe

```sh
Scripts/probe-metalxr-host-encoder.sh
```

The probe encodes 30 stereo frames at 30 fps and writes:

- `<prefix>_left.h264`
- `<prefix>_right.h264`
- `<prefix>_metadata.jsonl`

It fails if either stream is empty, if fewer than 60 eye frames are encoded, if summary records are missing, or if any drop record is emitted.

The stream probe exercises the TCP transport path:

```sh
Scripts/probe-metalxr-frame-stream.sh
```

It starts `metalxr_host_streamer` with the synthetic frame source, with a generated file-payload Unity-export fixture, with a generated IOSurface Unity-export fixture, with a generated Unix-datagram frame-record fixture, and with a reconnecting synthetic client. Each run performs the Quest HELLO/HELLO_ACK exchange, responds to timing sync probes, receives finite `METALXR_PACKET_VIDEO_FRAME` packets, verifies left/right eye counts, and checks that each packet carries H.264 bytes after the fixed video-frame metadata. The probe also verifies clock-sync, latency, reconnect, frame-source JSON records, the host's `IOSurfaceLookup`/`CVPixelBufferCreateWithIOSurface` path, and the socket metadata path.

For a headset session, launch the Quest client APK and then run:

```sh
Scripts/run-metalxr-frame-stream.sh
```

USB development uses `adb reverse tcp:47000 tcp:47000` and binds the streamer to `127.0.0.1`. While the streamer is running, the launch script refreshes `adb reverse` every `METALXR_ADB_REVERSE_REFRESH_SECONDS` seconds (default 5, set 0 to disable) so common USB or Quest-client restarts can reconnect without restarting the Mac-side session. Wi-Fi tests can use `METALXR_TRANSPORT=wifi`, which binds to `0.0.0.0`; launch the Quest client with a reachable host address through the `metalxr_host` intent extra or `--metalxr-host` argument.

By default the streamer uses synthetic frames. To stream Unity-exported runtime frames, launch Unity with `METALXR_FRAME_EXPORT_DIR` set and then run:

```sh
METALXR_FRAME_SOURCE=unity-export \
METALXR_FRAME_EXPORT_DIR=/tmp/metalxr_frame_export \
Scripts/run-metalxr-frame-stream.sh
```

The streamer reads `<export-dir>/frames.jsonl`, selects the latest complete left/right eye pair, auto-configures the encoder dimensions from that pair, validates those dimensions against the Quest HELLO device profile, and logs frame age plus repeated-frame counts. As a step away from file-index polling, the streamer can also bind a Unix datagram socket with `METALXR_FRAME_EXPORT_SOCKET=/tmp/metalxr_frame_export.sock`; the runtime sends the same JSONL frame records to that socket while still writing file records for debug capture. Raw `BGRA8` and `RGBA8` records still read a payload file into the encoder pool. Experimental `IOSurfaceBGRA8` and `IOSurfaceRGBA8` records use `ioSurfaceId`, `IOSurfaceLookup`, and `CVPixelBufferCreateWithIOSurface` so the host can submit a VideoToolbox input buffer without reading a frame payload file. If no complete pair is available when the client connects, it exits with a setup error instead of silently falling back to synthetic content.

For normal Play Mode sessions, `METALXR_STREAM_FRAMES=0` keeps the streamer alive and waits for a Quest client to reconnect after the app restarts or USB transport drops. Finite smoke tests can set `METALXR_STREAM_RECONNECT_ATTEMPTS=N`; the streamer logs `client_disconnect` and `reconnect_wait` records before accepting the next client. When `METALXR_STREAM_FPS` is not set, the host uses the Quest HELLO `preferredFps`; explicit FPS values are capped to the client preferred rate. `METALXR_STREAM_QUEUE_DEPTH`, `METALXR_STREAM_BITRATE`, resolution, frame rate, `METALXR_PREDICTION_OFFSET_MS`, and `METALXR_CLOCK_SYNC_INTERVAL_MS` remain the primary tuning controls for latency and backlog.

The host creates a POSIX shared-state object named by `METALXR_SHARED_STATE_NAME` (default `/metalxr_runtime_state`) for runtime-facing tracking, timing, and haptic state. `METALXR_DISABLE_SHARED_STATE=1` keeps the older text-file bridge as the only state path for debugging.

## Metadata

Each encoded frame emits one JSONL record:

```json
{"event":"encoded","frame":0,"eye":0,"eye_name":"left","width":320,"height":180,"timestamp_ns":0,"predicted_display_time_ns":33333333,"latency_us":2500,"bytes":2048}
```

At shutdown, each eye emits a summary record with submitted frames, encoded frames, dropped frames, output bytes, average latency, and max latency.

The streamer also emits `frame_source`, `clock_sync`, `latency`, `client_disconnect`, and `reconnect_wait` JSON records. Latency records break frame timing into encode, network, decode, compositor-submit, total, prediction error, queue depth, and measured frame period.

## Current Limitations

- Unity-rendered frames use development payload files by default before being copied into IOSurface-backed `CVPixelBufferPool` encoder slots. Frame metadata can now travel over a Unix datagram socket, but raw payload mode still references payload files. The host can consume runtime-emitted IOSurface ids, but that runtime sidecar path is still explicit and experimental. Runtime IOSurface export requires `METALXR_ENABLE_EXPERIMENTAL_IOSURFACE_EXPORT=1` in addition to `METALXR_FRAME_EXPORT_MODE=iosurface`.
- The encoder uses one H.264 session per eye. A future transport can either keep separate eye streams or add a stereo packing step.
- There is no HEVC path yet.
- There is no production GPU synchronization or direct host-owned frame-slot lifecycle yet.
- The stream path is TCP-only for now. USB adb reverse and Wi-Fi are both usable for evaluation, but automatic bitrate adaptation, packet loss handling, and production-grade clock sync are still future work.

## Next Step

The remaining frame work is replacing raw payload files with a lower-latency IOSurface handoff or a Metal blit into the host's VideoToolbox-compatible pixel buffer pool, then replacing the socket prototype with synchronized runtime/host ownership for frame slots. The remaining stream work is adaptive stream policy and reconnect-friendly control/media separation.
