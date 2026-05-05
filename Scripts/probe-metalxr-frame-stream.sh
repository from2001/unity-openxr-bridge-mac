#!/usr/bin/env bash
set -euo pipefail

# Probe the host frame streamer with an in-process Quest-like TCP client.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
streamer="$repo_root/Runtime/MetalXRHost/build/metalxr_host_streamer"
port="${METALXR_PROBE_STREAM_PORT:-47002}"
frames="${METALXR_PROBE_STREAM_FRAMES:-8}"
log_file="${TMPDIR:-/tmp}/metalxr_host_streamer_probe.log"

if [[ ! -x "$streamer" ]]; then
  "$repo_root/Scripts/build-metalxr-host.sh"
fi

python_bin="$(command -v python3 || true)"
if [[ -z "$python_bin" ]]; then
  echo "python3 is required for the frame stream probe." >&2
  exit 1
fi

rm -f "$log_file"

"$streamer" \
  --bind-host 127.0.0.1 \
  --port "$port" \
  --frames "$frames" \
  --fps 30 \
  --width 160 \
  --height 90 \
  --bitrate 800000 \
  --no-realtime >"$log_file" 2>&1 &
streamer_pid="$!"

cleanup() {
  if kill -0 "$streamer_pid" >/dev/null 2>&1; then
    kill "$streamer_pid" >/dev/null 2>&1 || true
    wait "$streamer_pid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

"$python_bin" - "$port" "$frames" <<'PY'
import socket
import struct
import sys
import time

port = int(sys.argv[1])
frames = int(sys.argv[2])
target_eye_frames = frames * 2

HEADER = struct.Struct("<IHHHHIQQII")
HELLO = struct.Struct("<IIIIIIII64s")
VIDEO = struct.Struct("<QIIIIQQQII")
MAGIC = 0x4D585250
HEADER_SIZE = 40
PACKET_HELLO = 1
PACKET_HELLO_ACK = 2
PACKET_VIDEO_FRAME = 10
ROLE_QUEST_CLIENT = 2
CAP_H264 = 0x00000001
CAP_STEREO_SEPARATE_EYES = 0x00000004
CAP_LOG_STREAM = 0x00000080


def recv_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        data = sock.recv(remaining)
        if not data:
            raise RuntimeError("socket closed")
        chunks.append(data)
        remaining -= len(data)
    return b"".join(chunks)


deadline = time.time() + 5.0
sock = None
while True:
    candidate = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        candidate.connect(("127.0.0.1", port))
        sock = candidate
        break
    except OSError:
        candidate.close()
        if time.time() >= deadline:
            raise
        time.sleep(0.05)

hello_payload = HELLO.pack(
    ROLE_QUEST_CLIENT,
    CAP_H264 | CAP_STEREO_SEPARATE_EYES | CAP_LOG_STREAM,
    2064,
    2208,
    72,
    port,
    0,
    0,
    b"MetalXR Probe Quest Client".ljust(64, b"\0"),
)
hello_header = HEADER.pack(MAGIC, HEADER_SIZE, PACKET_HELLO, 0, 1, 0, 1, time.monotonic_ns(), len(hello_payload), 0)
sock.sendall(hello_header + hello_payload)

ack_header = HEADER.unpack(recv_exact(sock, HEADER_SIZE))
if ack_header[0] != MAGIC or ack_header[2] != PACKET_HELLO_ACK:
    raise RuntimeError(f"expected HELLO_ACK, got type={ack_header[2]} magic=0x{ack_header[0]:08x}")
recv_exact(sock, ack_header[8])

eye_counts = {0: 0, 1: 0}
keyframes = 0
payload_bytes = 0
while eye_counts[0] + eye_counts[1] < target_eye_frames:
    raw_header = recv_exact(sock, HEADER_SIZE)
    header = HEADER.unpack(raw_header)
    if header[0] != MAGIC:
        raise RuntimeError(f"bad magic 0x{header[0]:08x}")
    payload = recv_exact(sock, header[8])
    if header[2] != PACKET_VIDEO_FRAME:
        continue
    if len(payload) < VIDEO.size:
        raise RuntimeError("short VIDEO_FRAME payload")

    video = VIDEO.unpack(payload[:VIDEO.size])
    frame_id, eye, codec, width, height, timestamp_ns, predicted_ns, encoder_latency_us, encoded_size, flags = video
    if codec != 1 or eye not in eye_counts:
        raise RuntimeError(f"bad video metadata codec={codec} eye={eye}")
    if width != 160 or height != 90:
        raise RuntimeError(f"bad stream size {width}x{height}")
    if encoded_size <= 0 or encoded_size > len(payload) - VIDEO.size:
        raise RuntimeError(f"bad encoded payload size {encoded_size}")
    eye_counts[eye] += 1
    payload_bytes += encoded_size
    if flags & 1:
        keyframes += 1
    if eye_counts[0] + eye_counts[1] <= 4:
        print(
            f"received frame={frame_id} eye={eye} bytes={encoded_size} "
            f"encoder_latency_us={encoder_latency_us}"
        )

sock.close()
print(f"received eye_frames={eye_counts[0] + eye_counts[1]} left={eye_counts[0]} right={eye_counts[1]} keyframes={keyframes} bytes={payload_bytes}")
PY

wait "$streamer_pid"
trap - EXIT

if grep -q '"event":"drop"' "$log_file"; then
  echo "Streamer reported dropped frames:" >&2
  grep '"event":"drop"' "$log_file" >&2
  exit 1
fi

echo "Host frame streamer log: $log_file"
sed -n '1,12p' "$log_file"
