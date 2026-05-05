# MetalXR Host Encoder

This document tracks the macOS host encoder added for issue #5.

## Purpose

The host encoder is an independent macOS command-line component that converts frame sources into a low-latency H.264 stream with VideoToolbox. The current source is synthetic stereo BGRA frames, which keeps the encoder path testable before the OpenXR runtime grows a real Metal readback or IOSurface handoff.

The implementation does not call Unity APIs. Frame generation, pixel buffer creation, and VideoToolbox encoding all run in the host process.

## Build

```sh
Scripts/build-metalxr-host.sh
```

The binary is built at:

```text
Runtime/MetalXRHost/build/metalxr_host_encoder
Runtime/MetalXRHost/build/metalxr_host_streamer
```

The build script uses CMake when available and falls back to direct `clang` compilation against:

- CoreFoundation
- CoreMedia
- CoreVideo
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

It starts `metalxr_host_streamer`, performs the Quest HELLO/HELLO_ACK exchange, responds to timing sync probes, receives finite `METALXR_PACKET_VIDEO_FRAME` packets, verifies left/right eye counts, and checks that each packet carries H.264 bytes after the fixed video-frame metadata. The probe also verifies clock-sync and latency JSON records.

For a headset session, launch the Quest client APK and then run:

```sh
Scripts/run-metalxr-frame-stream.sh
```

USB development uses `adb reverse tcp:47000 tcp:47000` and binds the streamer to `127.0.0.1`. Wi-Fi tests can use `METALXR_TRANSPORT=wifi`, which binds to `0.0.0.0`; launch the Quest client with a reachable host address through the `metalxr_host` intent extra or `--metalxr-host` argument.

## Metadata

Each encoded frame emits one JSONL record:

```json
{"event":"encoded","frame":0,"eye":0,"eye_name":"left","width":320,"height":180,"timestamp_ns":0,"predicted_display_time_ns":33333333,"latency_us":2500,"bytes":2048}
```

At shutdown, each eye emits a summary record with submitted frames, encoded frames, dropped frames, output bytes, average latency, and max latency.

The streamer also emits `clock_sync` and `latency` JSON records. Latency records break frame timing into encode, network, decode, compositor-submit, total, prediction error, queue depth, and measured frame period.

## Current Limitations

- The input is synthetic `CVPixelBuffer` data, not a Unity-rendered Metal texture.
- The encoder uses one H.264 session per eye. A future transport can either keep separate eye streams or add a stereo packing step.
- There is no HEVC path yet.
- There is no GPU synchronization, Metal blit, IOSurface export, or CPU readback from the OpenXR swapchain yet.
- The stream path is TCP-only for now. USB adb reverse and Wi-Fi are both usable for evaluation, but adaptive bitrate, packet loss handling, and production-grade clock sync are still future work.

## Next Step

The runtime and host streamer can be joined by adding a frame-source abstraction that accepts runtime-owned Metal textures through an IOSurface-backed path or a Metal blit into VideoToolbox-compatible pixel buffers.
