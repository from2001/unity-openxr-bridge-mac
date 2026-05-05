# MetalXR Protocol

This document defines the initial host/client protocol added across the streaming and input bridge milestones.

## Transport Choice

The initial control transport is a reliable byte stream. The development probe uses `socketpair` for in-process loopback; production should use TCP for Wi-Fi and adb reverse/forward for USB development.

The first media transport reuses the reliable TCP stream after HELLO/HELLO_ACK. H.264 frame metadata is followed by the encoded access-unit bytes in the same `METALXR_PACKET_VIDEO_FRAME` payload. This keeps USB adb reverse and Wi-Fi testing simple; payload delivery can move to UDP or a QUIC-like datagram path after packet loss behavior can be measured.

## Versioning

Every packet starts with `MetalXRPacketHeader` from:

```text
Runtime/MetalXRProtocol/include/MetalXRProtocol/metalxr_protocol.h
```

The header contains:

- `magic`: fixed `METALXR_PROTOCOL_MAGIC`.
- `headerSize`: byte size of `MetalXRPacketHeader`.
- `type`: packet type.
- `versionMajor` and `versionMinor`: protocol version.
- `sequence`: monotonic sender sequence number.
- `timestampNs`: sender monotonic timestamp in nanoseconds.
- `payloadSize`: byte size of the payload following the header.

Major version mismatch is fatal and returns `METALXR_PACKET_ERROR` with `METALXR_ERROR_VERSION_MISMATCH`. Minor version differences are reserved for future backward-compatible capability negotiation.

## Handshake

The Quest client sends `METALXR_PACKET_HELLO` with `MetalXRHelloPayload`:

- `role`: host or Quest client.
- `capabilities`: codec, stereo packing, pose/controller/haptics/log capability flags.
- `maxVideoWidth` and `maxVideoHeight`.
- `preferredFps`.
- `controlPort` and `mediaPort`.
- `deviceName`.

The host replies with `METALXR_PACKET_HELLO_ACK` using the same payload shape and the negotiated host capabilities.

## Heartbeat

Both endpoints can send `METALXR_PACKET_HEARTBEAT` with:

- `sessionId`: negotiated session id.
- `monotonicTimeNs`: local monotonic timestamp.
- `lastFrameId`: last processed frame id.
- `batteryPercent`: client battery when known.
- `flags`: reserved status flags.

## Media And Timing Packets

`METALXR_PACKET_VIDEO_FRAME` metadata contains:

- `frameId`
- `eye`
- `codec`
- `width` and `height`
- `timestampNs`: frame capture or source timestamp.
- `predictedDisplayTimeNs`: predicted display time from the OpenXR frame loop.
- `encoderLatencyUs`
- `payloadBytes`
- `flags`

The packet payload layout is:

```text
MetalXRPacketHeader
MetalXRVideoFramePayload
payloadBytes of H.264 Annex B data
```

`flags & 0x1` marks keyframes. Keyframe packets include SPS/PPS parameter sets before the access unit so the Quest-side decoder can recover after reconnects.

`METALXR_PACKET_TIMING_SAMPLE` separates timing telemetry from media payloads and includes host capture, predicted display, encode start/end, client receive, and client display timestamps. All timing fields are monotonic nanoseconds in the sender's local clock domain unless a later clock-sync packet defines a shared mapping.

## Pose, Controller, And Haptic Packets

`METALXR_PACKET_POSE_SAMPLE` carries one Quest HMD sample:

- `sampleId`
- `timestampNs`
- `predictedDisplayTimeNs`
- `position[3]`
- `orientation[4]`
- `trackingFlags`

`trackingFlags` uses `METALXR_TRACKING_ORIENTATION_VALID`, `METALXR_TRACKING_POSITION_VALID`, `METALXR_TRACKING_ORIENTATION_TRACKED`, and `METALXR_TRACKING_POSITION_TRACKED`.

`METALXR_PACKET_CONTROLLER_INPUT` carries one controller sample:

- `sampleId`
- `timestampNs`
- `hand`: `METALXR_CONTROLLER_HAND_LEFT` or `METALXR_CONTROLLER_HAND_RIGHT`
- `buttons`: primary, secondary, menu, and thumbstick button flags
- `trigger`, `grip`, and `thumbstick[2]`
- `trackingFlags`
- `aimPosition[3]` and `aimOrientation[4]`
- `gripPosition[3]` and `gripOrientation[4]`

The current Quest client fills aim and grip poses from the tracked XR device pose when separate aim/grip data is not available through the simple Unity XR device path.

`METALXR_PACKET_HAPTIC_COMMAND` flows from host to Quest:

- `commandId`
- `timestampNs`
- `hand`
- `amplitude`
- `frequencyHz`
- `durationUs`

The development host bridge writes the latest HMD/controller state to `METALXR_TRACKING_STATE_PATH` and polls `METALXR_HAPTIC_COMMAND_PATH` for haptic commands emitted by the OpenXR runtime. These files are a local integration bridge, not the final transport format.

## Probe

```sh
Scripts/build-metalxr-protocol.sh
Scripts/probe-metalxr-protocol.sh
```

The probe verifies:

- Client hello to host.
- Host hello ack to client.
- Client heartbeat to host.
- Host heartbeat reply to client.
- Major protocol version mismatch returns a clear `VERSION_MISMATCH` error.
- Runtime-side HMD pose, action state, action-space, and haptic output are covered by `Scripts/probe-metalxr-input-bridge.sh`.

## Current Limitations

- The loopback uses an in-process socket pair, not adb or Wi-Fi.
- Packet structs are fixed-layout C structs with matching Unity C# packing helpers; generated bindings are still needed.
- There is no clock synchronization packet yet; timing fields are explicit but remain in local monotonic clock domains.
- TCP media transport is a development path. Datagram transport, adaptive pacing, and retransmission policy are not defined yet.
- Tracking and haptics use state-file handoff between the host streamer and runtime while the real host/runtime integration is still being built.
