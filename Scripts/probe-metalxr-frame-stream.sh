#!/usr/bin/env bash
set -euo pipefail

# Probe the host frame streamer with an in-process Quest-like TCP client.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
streamer="$repo_root/Runtime/MetalXRHost/build/metalxr_host_streamer"
iosurface_fixture="$repo_root/Runtime/MetalXRHost/build/metalxr_iosurface_export_fixture"
base_port="${METALXR_PROBE_STREAM_PORT:-47002}"
frames="${METALXR_PROBE_STREAM_FRAMES:-8}"
log_dir="${TMPDIR:-/tmp}"

if [[ ! -x "$streamer" || ! -x "$iosurface_fixture" ]]; then
  "$repo_root/Scripts/build-metalxr-host.sh"
fi

python_bin="$(command -v python3 || true)"
if [[ -z "$python_bin" ]]; then
  echo "python3 is required for the frame stream probe." >&2
  exit 1
fi

make_unity_export_fixture() {
  local fixture_dir="$1"
  local width="$2"
  local height="$3"
  local source_frames="$4"

  "$python_bin" - "$fixture_dir" "$width" "$height" "$source_frames" <<'PY'
import json
import pathlib
import sys
import time

fixture_dir = pathlib.Path(sys.argv[1])
width = int(sys.argv[2])
height = int(sys.argv[3])
source_frames = int(sys.argv[4])
fixture_dir.mkdir(parents=True, exist_ok=True)
index_path = fixture_dir / "frames.jsonl"
index_path.write_text("")
display_base = time.monotonic_ns()

for frame in range(source_frames):
    display_time = display_base + ((frame + 1) * 16_666_666)
    for eye in range(2):
        payload_path = fixture_dir / f"frame_{frame:06d}_eye_{eye}.bgra"
        payload = bytearray(width * height * 4)
        for y in range(height):
            for x in range(width):
                offset = ((y * width) + x) * 4
                payload[offset + 0] = (x + frame * 11 + eye * 41) & 0xFF
                payload[offset + 1] = (y + frame * 7) & 0xFF
                payload[offset + 2] = (x + y + eye * 80) & 0xFF
                payload[offset + 3] = 255
        payload_path.write_bytes(payload)
        record = {
            "event": "frame_export",
            "frame": frame,
            "eye": eye,
            "displayTime": display_time,
            "swapchain": 1,
            "imageIndex": 0,
            "texture": "fixture",
            "pixelFormat": 80,
            "payloadFormat": "BGRA8",
            "width": width,
            "height": height,
            "bytesPerRow": width * 4,
            "payloadBytes": len(payload),
            "sourceRect": {"x": 0, "y": 0, "width": width, "height": height},
            "arrayIndex": 0,
            "storageMode": 1,
            "mode": "fixture",
            "payloadPath": str(payload_path),
        }
        line = json.dumps(record, separators=(",", ":"))
        (fixture_dir / f"frame_{frame:06d}_eye_{eye}.json").write_text(line + "\n")
        with index_path.open("a") as index:
            index.write(line + "\n")
PY
}

run_stream_probe() {
  local name="$1"
  local port="$2"
  local width="$3"
  local height="$4"
  local frame_source="$5"
  local frame_export_dir="${6:-}"
  local frame_export_socket="${7:-}"
  local socket_fixture_dir="${8:-}"
  local frame_export_ack_socket="${9:-}"
  local log_file="$log_dir/metalxr_host_streamer_probe_${name}.log"

  rm -f "$log_file"

  local streamer_args=(
    --bind-host 127.0.0.1
    --port "$port"
    --frames "$frames"
    --fps 30
    --width "$width"
    --height "$height"
    --bitrate 800000
    --queue-depth 32
    --frame-source "$frame_source"
    --no-realtime
  )

  if [[ -n "$frame_export_dir" ]]; then
    streamer_args+=(--frame-export-dir "$frame_export_dir")
  fi
  if [[ -n "$frame_export_socket" ]]; then
    streamer_args+=(--frame-export-socket "$frame_export_socket")
  fi
  if [[ -n "$frame_export_ack_socket" ]]; then
    streamer_args+=(--frame-export-ack-socket "$frame_export_ack_socket")
  fi

  "$streamer" "${streamer_args[@]}" >"$log_file" 2>&1 &
  local streamer_pid="$!"

  set +e
  "$python_bin" - "$port" "$frames" "$width" "$height" "$name" "$frame_export_socket" "$socket_fixture_dir" <<'PY'
import pathlib
import socket
import struct
import sys
import time

port = int(sys.argv[1])
frames = int(sys.argv[2])
expected_width = int(sys.argv[3])
expected_height = int(sys.argv[4])
probe_name = sys.argv[5]
frame_export_socket = sys.argv[6]
socket_fixture_dir = pathlib.Path(sys.argv[7]) if len(sys.argv) > 7 and sys.argv[7] else None
target_eye_frames = frames * 2

HEADER = struct.Struct("<IHHHHIQQII")
HELLO = struct.Struct("<IIIIIIII64s")
POSE = struct.Struct("<QQQ3f4fII")
CONTROLLER = struct.Struct("<QQIIff2fII3f4f3f4f")
VIDEO = struct.Struct("<QIIIIQQQII")
TIMING = struct.Struct("<QQQQQQQQQQII")
MAGIC = 0x4D585250
HEADER_SIZE = 40
PACKET_HELLO = 1
PACKET_HELLO_ACK = 2
PACKET_VIDEO_FRAME = 10
PACKET_POSE_SAMPLE = 20
PACKET_CONTROLLER_INPUT = 21
PACKET_TIMING_SAMPLE = 30
TIMING_FLAG_CLOCK_SYNC = 0x00000001
TIMING_FLAG_FRAME_DISPLAY = 0x00000002
ROLE_QUEST_CLIENT = 2
CAP_H264 = 0x00000001
CAP_STEREO_SEPARATE_EYES = 0x00000004
CAP_POSE_INPUT = 0x00000010
CAP_CONTROLLER_INPUT = 0x00000020
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


sequence = 2


def send_packet(sock, packet_type, payload):
    global sequence
    header = HEADER.pack(MAGIC, HEADER_SIZE, packet_type, 0, 1, 0, sequence, time.monotonic_ns(), len(payload), 0)
    sequence += 1
    sock.sendall(header + payload)


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
    CAP_H264 | CAP_STEREO_SEPARATE_EYES | CAP_POSE_INPUT | CAP_CONTROLLER_INPUT | CAP_LOG_STREAM,
    2064,
    2208,
    72,
    port,
    0,
    0,
    b"MetalXR Probe Quest Client".ljust(64, b"\0"),
)
send_packet(sock, PACKET_HELLO, hello_payload)

if frame_export_socket:
    if socket_fixture_dir is None:
        raise RuntimeError("socket fixture dir is required when frame export socket is set")
    socket_deadline = time.time() + 3.0
    socket_path = pathlib.Path(frame_export_socket)
    while not socket_path.exists():
        if time.time() >= socket_deadline:
            raise RuntimeError(f"frame export socket did not appear: {socket_path}")
        time.sleep(0.01)
    datagram = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        lines = (socket_fixture_dir / "frames.jsonl").read_text().splitlines()
        for line in lines:
            payload = line.encode("utf-8")
            send_deadline = time.time() + 2.0
            while True:
                try:
                    datagram.sendto(payload, frame_export_socket)
                    break
                except OSError as exc:
                    if getattr(exc, "errno", None) != 55 or time.time() >= send_deadline:
                        raise
                    time.sleep(0.01)
            time.sleep(0.005)
    finally:
        datagram.close()

ack_header = HEADER.unpack(recv_exact(sock, HEADER_SIZE))
if ack_header[0] != MAGIC or ack_header[2] != PACKET_HELLO_ACK:
    raise RuntimeError(f"expected HELLO_ACK, got type={ack_header[2]} magic=0x{ack_header[0]:08x}")
recv_exact(sock, ack_header[8])

input_ns = time.monotonic_ns()
send_packet(
    sock,
    PACKET_POSE_SAMPLE,
    POSE.pack(1001, input_ns, 0, 0.06, 1.22, 0.20, 0.0, 0.0, 0.0, 1.0, 0x0F, 0),
)
for hand in (0, 1):
    x = -0.2 if hand == 0 else 0.2
    send_packet(
        sock,
        PACKET_CONTROLLER_INPUT,
        CONTROLLER.pack(
            1002 + hand,
            input_ns,
            hand,
            0,
            0.0,
            0.0,
            0.0,
            0.0,
            0x0F,
            0,
            x,
            1.1,
            0.1,
            0.0,
            0.0,
            0.0,
            1.0,
            x,
            1.0,
            0.1,
            0.0,
            0.0,
            0.0,
            1.0,
        ),
    )

eye_counts = {0: 0, 1: 0}
keyframes = 0
payload_bytes = 0
while eye_counts[0] + eye_counts[1] < target_eye_frames:
    raw_header = recv_exact(sock, HEADER_SIZE)
    header = HEADER.unpack(raw_header)
    if header[0] != MAGIC:
        raise RuntimeError(f"bad magic 0x{header[0]:08x}")
    payload = recv_exact(sock, header[8])
    receive_ns = time.monotonic_ns()
    if header[2] == PACKET_TIMING_SAMPLE:
        if len(payload) != TIMING.size:
            raise RuntimeError("bad TIMING_SAMPLE payload")
        timing = TIMING.unpack(payload)
        if timing[11] & TIMING_FLAG_CLOCK_SYNC:
            now_ns = time.monotonic_ns()
            response = TIMING.pack(
                timing[0],
                timing[1],
                timing[2],
                timing[3],
                timing[4],
                now_ns,
                now_ns,
                0,
                0,
                now_ns,
                0,
                TIMING_FLAG_CLOCK_SYNC,
            )
            send_packet(sock, PACKET_TIMING_SAMPLE, response)
        continue
    if header[2] != PACKET_VIDEO_FRAME:
        continue
    if len(payload) < VIDEO.size:
        raise RuntimeError("short VIDEO_FRAME payload")

    video = VIDEO.unpack(payload[:VIDEO.size])
    frame_id, eye, codec, width, height, timestamp_ns, predicted_ns, encoder_latency_us, encoded_size, flags = video
    if codec != 1 or eye not in eye_counts:
        raise RuntimeError(f"bad video metadata codec={codec} eye={eye}")
    if width != expected_width or height != expected_height:
        raise RuntimeError(f"bad stream size {width}x{height}; expected {expected_width}x{expected_height}")
    if encoded_size <= 0 or encoded_size > len(payload) - VIDEO.size:
        raise RuntimeError(f"bad encoded payload size {encoded_size}")
    eye_counts[eye] += 1
    payload_bytes += encoded_size
    if flags & 1:
        keyframes += 1
    encode_end_ns = header[7]
    encode_start_ns = encode_end_ns - (encoder_latency_us * 1000) if encode_end_ns >= encoder_latency_us * 1000 else 0
    decode_start_ns = receive_ns
    decode_end_ns = decode_start_ns + 100000
    display_ns = decode_end_ns + 100000
    display_timing = TIMING.pack(
        frame_id,
        timestamp_ns,
        predicted_ns,
        encode_start_ns,
        encode_end_ns,
        receive_ns,
        display_ns,
        decode_start_ns,
        decode_end_ns,
        display_ns,
        0,
        TIMING_FLAG_FRAME_DISPLAY,
    )
    send_packet(sock, PACKET_TIMING_SAMPLE, display_timing)
    if eye_counts[0] + eye_counts[1] <= 4:
        print(
            f"{probe_name}: received frame={frame_id} eye={eye} bytes={encoded_size} "
            f"encoder_latency_us={encoder_latency_us}"
        )

sock.close()
print(f"{probe_name}: received eye_frames={eye_counts[0] + eye_counts[1]} left={eye_counts[0]} right={eye_counts[1]} keyframes={keyframes} bytes={payload_bytes}")
PY
  local client_status="$?"
  set -e

  if [[ "$client_status" -ne 0 ]]; then
    if kill -0 "$streamer_pid" >/dev/null 2>&1; then
      kill "$streamer_pid" >/dev/null 2>&1 || true
      wait "$streamer_pid" >/dev/null 2>&1 || true
    fi
    cat "$log_file" >&2
    exit "$client_status"
  fi

  wait "$streamer_pid"

  if grep -q '"event":"drop"' "$log_file"; then
    echo "Streamer reported dropped frames in $name:" >&2
    grep '"event":"drop"' "$log_file" >&2
    exit 1
  fi

  if ! grep -q '"event":"clock_sync"' "$log_file"; then
    echo "Streamer did not report clock sync in $name." >&2
    cat "$log_file" >&2
    exit 1
  fi

  if ! grep -q '"event":"latency"' "$log_file"; then
    echo "Streamer did not report latency samples in $name." >&2
    cat "$log_file" >&2
    exit 1
  fi

  if ! grep -q '"event":"pose"' "$log_file" || ! grep -q '"event":"controller"' "$log_file"; then
    echo "Streamer did not process probe input packets in $name." >&2
    cat "$log_file" >&2
    exit 1
  fi

  if ! grep -q '"event":"pixel_buffer_pool"' "$log_file"; then
    echo "Streamer did not create encoder pixel buffer pools in $name." >&2
    cat "$log_file" >&2
    exit 1
  fi

  if [[ "$frame_source" == "synthetic" ]]; then
    grep -q 'source=synthetic' "$log_file"
  else
    grep -q '"source":"unity-export"' "$log_file"
  fi

  echo "Host frame streamer $name log: $log_file"
  sed -n '1,18p' "$log_file"
}

run_reconnect_probe() {
  local port="$1"
  local width=128
  local height=72
  local reconnect_frames=4
  local log_file="$log_dir/metalxr_host_streamer_probe_reconnect.log"

  rm -f "$log_file"
  "$streamer" \
    --bind-host 127.0.0.1 \
    --port "$port" \
    --frames "$reconnect_frames" \
    --fps 30 \
    --width "$width" \
    --height "$height" \
    --bitrate 800000 \
    --queue-depth 32 \
    --reconnect-attempts 1 \
    --frame-source synthetic \
    --no-realtime >"$log_file" 2>&1 &
  local streamer_pid="$!"

  set +e
  "$python_bin" - "$port" "$reconnect_frames" "$width" "$height" <<'PY'
import socket
import struct
import sys
import time

port = int(sys.argv[1])
frames = int(sys.argv[2])
expected_width = int(sys.argv[3])
expected_height = int(sys.argv[4])

HEADER = struct.Struct("<IHHHHIQQII")
HELLO = struct.Struct("<IIIIIIII64s")
VIDEO = struct.Struct("<QIIIIQQQII")
TIMING = struct.Struct("<QQQQQQQQQQII")
MAGIC = 0x4D585250
HEADER_SIZE = 40
PACKET_HELLO = 1
PACKET_HELLO_ACK = 2
PACKET_VIDEO_FRAME = 10
PACKET_TIMING_SAMPLE = 30
TIMING_FLAG_CLOCK_SYNC = 0x00000001
TIMING_FLAG_FRAME_DISPLAY = 0x00000002
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


def connect_socket():
    deadline = time.time() + 5.0
    while True:
        candidate = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            candidate.connect(("127.0.0.1", port))
            return candidate
        except OSError:
            candidate.close()
            if time.time() >= deadline:
                raise
            time.sleep(0.05)


def send_packet(sock, sequence, packet_type, payload):
    header = HEADER.pack(MAGIC, HEADER_SIZE, packet_type, 0, 1, 0, sequence, time.monotonic_ns(), len(payload), 0)
    sock.sendall(header + payload)
    return sequence + 1


def connect_and_hello():
    sock = connect_socket()
    payload = HELLO.pack(
        ROLE_QUEST_CLIENT,
        CAP_H264 | CAP_STEREO_SEPARATE_EYES | CAP_LOG_STREAM,
        2064,
        2208,
        72,
        port,
        0,
        0,
        b"MetalXR Reconnect Probe".ljust(64, b"\0"),
    )
    sequence = send_packet(sock, 2, PACKET_HELLO, payload)
    ack_header = HEADER.unpack(recv_exact(sock, HEADER_SIZE))
    if ack_header[0] != MAGIC or ack_header[2] != PACKET_HELLO_ACK:
        raise RuntimeError(f"expected HELLO_ACK, got type={ack_header[2]} magic=0x{ack_header[0]:08x}")
    recv_exact(sock, ack_header[8])
    return sock, sequence


first, _ = connect_and_hello()
first.close()
time.sleep(0.2)

sock, sequence = connect_and_hello()
target_eye_frames = frames * 2
eye_counts = {0: 0, 1: 0}
while eye_counts[0] + eye_counts[1] < target_eye_frames:
    header = HEADER.unpack(recv_exact(sock, HEADER_SIZE))
    if header[0] != MAGIC:
        raise RuntimeError(f"bad magic 0x{header[0]:08x}")
    payload = recv_exact(sock, header[8])
    receive_ns = time.monotonic_ns()
    if header[2] == PACKET_TIMING_SAMPLE:
        timing = TIMING.unpack(payload)
        if timing[11] & TIMING_FLAG_CLOCK_SYNC:
            now_ns = time.monotonic_ns()
            response = TIMING.pack(
                timing[0],
                timing[1],
                timing[2],
                timing[3],
                timing[4],
                now_ns,
                now_ns,
                0,
                0,
                now_ns,
                0,
                TIMING_FLAG_CLOCK_SYNC,
            )
            sequence = send_packet(sock, sequence, PACKET_TIMING_SAMPLE, response)
        continue
    if header[2] != PACKET_VIDEO_FRAME:
        continue
    video = VIDEO.unpack(payload[:VIDEO.size])
    frame_id, eye, codec, width, height, timestamp_ns, predicted_ns, encoder_latency_us, encoded_size, flags = video
    if codec != 1 or eye not in eye_counts:
        raise RuntimeError(f"bad video metadata codec={codec} eye={eye}")
    if width != expected_width or height != expected_height:
        raise RuntimeError(f"bad stream size {width}x{height}; expected {expected_width}x{expected_height}")
    if encoded_size <= 0 or encoded_size > len(payload) - VIDEO.size:
        raise RuntimeError(f"bad encoded payload size {encoded_size}")
    eye_counts[eye] += 1
    encode_end_ns = header[7]
    encode_start_ns = encode_end_ns - (encoder_latency_us * 1000) if encode_end_ns >= encoder_latency_us * 1000 else 0
    decode_start_ns = receive_ns
    decode_end_ns = decode_start_ns + 100000
    display_ns = decode_end_ns + 100000
    response = TIMING.pack(
        frame_id,
        timestamp_ns,
        predicted_ns,
        encode_start_ns,
        encode_end_ns,
        receive_ns,
        display_ns,
        decode_start_ns,
        decode_end_ns,
        display_ns,
        0,
        TIMING_FLAG_FRAME_DISPLAY,
    )
    sequence = send_packet(sock, sequence, PACKET_TIMING_SAMPLE, response)

sock.close()
print(f"reconnect: received eye_frames={eye_counts[0] + eye_counts[1]} left={eye_counts[0]} right={eye_counts[1]}")
PY
  local client_status="$?"
  set -e

  if [[ "$client_status" -ne 0 ]]; then
    if kill -0 "$streamer_pid" >/dev/null 2>&1; then
      kill "$streamer_pid" >/dev/null 2>&1 || true
      wait "$streamer_pid" >/dev/null 2>&1 || true
    fi
    cat "$log_file" >&2
    exit "$client_status"
  fi

  wait "$streamer_pid"

  if ! grep -q '"event":"client_disconnect"' "$log_file"; then
    echo "Streamer did not report a reconnectable client disconnect." >&2
    cat "$log_file" >&2
    exit 1
  fi
  if ! grep -q '"event":"reconnect_wait"' "$log_file"; then
    echo "Streamer did not wait for reconnect." >&2
    cat "$log_file" >&2
    exit 1
  fi
  if ! grep -q '"event":"streamed"' "$log_file"; then
    echo "Streamer did not resume streaming after reconnect." >&2
    cat "$log_file" >&2
    exit 1
  fi

  echo "Host frame streamer reconnect log: $log_file"
  sed -n '1,24p' "$log_file"
}

run_iosurface_export_probe() {
  local port="$1"
  local width=96
  local height=64
  local fixture_dir
  local fixture_log
  fixture_dir="$(mktemp -d "${TMPDIR:-/tmp}/metalxr_iosurface_export_probe.XXXXXX")"
  fixture_log="$log_dir/metalxr_iosurface_export_fixture.log"
  rm -f "$fixture_log"

  "$iosurface_fixture" "$fixture_dir" "$width" "$height" 3 20 >"$fixture_log" 2>&1 &
  local fixture_pid="$!"

  local ready=0
  for _ in $(seq 1 50); do
    if [[ -f "$fixture_dir/frames.jsonl" ]] &&
        grep -q '"payloadFormat":"IOSurfaceBGRA8"' "$fixture_dir/frames.jsonl"; then
      ready=1
      break
    fi
    if ! kill -0 "$fixture_pid" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done

  if [[ "$ready" != "1" ]]; then
    echo "IOSurface export fixture did not become ready." >&2
    cat "$fixture_log" >&2 || true
    if kill -0 "$fixture_pid" >/dev/null 2>&1; then
      kill "$fixture_pid" >/dev/null 2>&1 || true
    fi
    wait "$fixture_pid" >/dev/null 2>&1 || true
    exit 1
  fi

  run_stream_probe "iosurface_export" "$port" "$width" "$height" "unity-export" "$fixture_dir"

  if kill -0 "$fixture_pid" >/dev/null 2>&1; then
    kill "$fixture_pid" >/dev/null 2>&1 || true
  fi
  wait "$fixture_pid" >/dev/null 2>&1 || true
  echo "Host frame streamer IOSurface fixture log: $fixture_log"
  sed -n '1,4p' "$fixture_log"
}

run_iosurface_socket_export_probe() {
  local port="$1"
  local width=96
  local height=64
  local fixture_dir
  local socket_path
  local ack_socket_path
  local ack_capture
  local fixture_log
  fixture_dir="$(mktemp -d "${TMPDIR:-/tmp}/metalxr_iosurface_socket_export_probe.XXXXXX")"
  socket_path="/tmp/metalxr-iosurface-socket-export-${port}-$$.sock"
  ack_socket_path="$socket_path.ack"
  ack_capture="$log_dir/metalxr_iosurface_socket_export_acks.jsonl"
  fixture_log="$log_dir/metalxr_iosurface_socket_export_fixture.log"
  rm -f "$fixture_log" "$socket_path" "$ack_socket_path" "$ack_capture"

  "$python_bin" - "$ack_socket_path" "$ack_capture" <<'PY' &
import pathlib
import socket
import sys
import time

socket_path = pathlib.Path(sys.argv[1])
capture_path = pathlib.Path(sys.argv[2])
try:
    socket_path.unlink()
except FileNotFoundError:
    pass

sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind(str(socket_path))
sock.settimeout(0.5)
deadline = time.time() + 10.0
captured = 0
with capture_path.open("w", encoding="utf-8") as output:
    while time.time() < deadline and captured < 2:
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        output.write(data.decode("utf-8") + "\n")
        output.flush()
        captured += 1
sock.close()
PY
  local ack_listener_pid="$!"
  for _ in $(seq 1 50); do
    if [[ -S "$ack_socket_path" ]]; then
      break
    fi
    sleep 0.05
  done

  "$iosurface_fixture" "$fixture_dir" "$width" "$height" 3 20 >"$fixture_log" 2>&1 &
  local fixture_pid="$!"

  local ready=0
  for _ in $(seq 1 50); do
    if [[ -f "$fixture_dir/frames.jsonl" ]] &&
        grep -q '"payloadFormat":"IOSurfaceBGRA8"' "$fixture_dir/frames.jsonl"; then
      ready=1
      break
    fi
    if ! kill -0 "$fixture_pid" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done

  if [[ "$ready" != "1" ]]; then
    echo "IOSurface socket export fixture did not become ready." >&2
    cat "$fixture_log" >&2 || true
    if kill -0 "$fixture_pid" >/dev/null 2>&1; then
      kill "$fixture_pid" >/dev/null 2>&1 || true
    fi
    wait "$fixture_pid" >/dev/null 2>&1 || true
    exit 1
  fi

  run_stream_probe "iosurface_socket_export" "$port" "$width" "$height" "unity-export" "" "$socket_path" "$fixture_dir" "$ack_socket_path"

  wait "$ack_listener_pid" || true
  if ! grep -q '"event":"frame_slot_release"' "$ack_capture"; then
    echo "IOSurface socket export probe did not receive frame slot release acks." >&2
    cat "$ack_capture" >&2 || true
    exit 1
  fi

  rm -f "$socket_path" "$ack_socket_path"
  if kill -0 "$fixture_pid" >/dev/null 2>&1; then
    kill "$fixture_pid" >/dev/null 2>&1 || true
  fi
  wait "$fixture_pid" >/dev/null 2>&1 || true
  echo "Host frame streamer IOSurface socket fixture log: $fixture_log"
  sed -n '1,4p' "$fixture_log"
}

run_socket_export_probe() {
  local port="$1"
  local width=96
  local height=64
  local fixture_dir
  local socket_path
  fixture_dir="$(mktemp -d "${TMPDIR:-/tmp}/metalxr_socket_export_probe.XXXXXX")"
  socket_path="/tmp/metalxr-socket-export-${port}-$$.sock"
  rm -f "$socket_path"
  make_unity_export_fixture "$fixture_dir" "$width" "$height" 3
  run_stream_probe "socket_export" "$port" "$width" "$height" "unity-export" "" "$socket_path" "$fixture_dir"
  rm -f "$socket_path"
}

unity_fixture_dir="$(mktemp -d "${TMPDIR:-/tmp}/metalxr_unity_export_probe.XXXXXX")"
make_unity_export_fixture "$unity_fixture_dir" 96 64 3

run_stream_probe "synthetic" "$base_port" 160 90 "synthetic"
run_stream_probe "unity_export" "$((base_port + 1))" 96 64 "unity-export" "$unity_fixture_dir"
run_iosurface_export_probe "$((base_port + 2))"
run_iosurface_socket_export_probe "$((base_port + 3))"
run_socket_export_probe "$((base_port + 4))"
run_reconnect_probe "$((base_port + 5))"
