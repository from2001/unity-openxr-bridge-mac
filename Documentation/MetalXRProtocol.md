# MetalXR Protocol

This document defines the initial host/client protocol for issue #6.

## Transport Choice

The initial control transport is a reliable byte stream. The development probe uses `socketpair` for in-process loopback; production should use TCP for Wi-Fi and adb reverse/forward for USB development.

Media transport is intentionally separated from the control channel. H.264 frame metadata is defined now, but payload delivery can move to UDP or a QUIC-like datagram path after the Quest client exists and packet loss behavior can be measured.

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

`METALXR_PACKET_TIMING_SAMPLE` separates timing telemetry from media payloads and includes host capture, predicted display, encode start/end, client receive, and client display timestamps. All timing fields are monotonic nanoseconds in the sender's local clock domain unless a later clock-sync packet defines a shared mapping.

Pose, controller, haptic, and log packet structs are declared in the shared header so host and Quest code can agree on binary layout before those subsystems are implemented.

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

## Current Limitations

- The loopback uses an in-process socket pair, not adb or Wi-Fi.
- Packet structs are fixed-layout C structs; cross-language bindings for Quest Android are still needed.
- Media payload framing is not implemented yet. The current packet only defines metadata.
- There is no clock synchronization packet yet; timing fields are explicit but remain in local monotonic clock domains.
