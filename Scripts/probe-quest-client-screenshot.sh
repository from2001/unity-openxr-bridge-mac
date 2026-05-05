#!/usr/bin/env bash
set -euo pipefail

# Capture a Quest screenshot and fail if the stream area appears black or colorless.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
adb_path="${ADB:-}"
output_path="${METALXR_SCREENSHOT_PATH:-${TMPDIR:-/tmp}/metalxr_quest_stream.png}"
min_bright_samples="${METALXR_SCREENSHOT_MIN_BRIGHT_SAMPLES:-8}"
min_color_samples="${METALXR_SCREENSHOT_MIN_COLOR_SAMPLES:-8}"

if [[ -z "$adb_path" && -x "$repo_root/Libs/adb-lib/adb" ]]; then
  adb_path="$repo_root/Libs/adb-lib/adb"
fi
if [[ -z "$adb_path" ]]; then
  adb_path="$(command -v adb || true)"
fi
if [[ -z "$adb_path" || ! -x "$adb_path" ]]; then
  echo "adb was not found. Set ADB=/path/to/adb." >&2
  exit 1
fi

python_bin="$(command -v python3 || true)"
if [[ -z "$python_bin" ]]; then
  echo "python3 is required for screenshot analysis." >&2
  exit 1
fi

mkdir -p "$(dirname "$output_path")"
"$adb_path" exec-out screencap -p >"$output_path"

"$python_bin" - "$output_path" "$min_bright_samples" "$min_color_samples" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
min_bright_samples = int(sys.argv[2])
min_color_samples = int(sys.argv[3])


def paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


with open(path, "rb") as handle:
    data = handle.read()

if not data.startswith(b"\x89PNG\r\n\x1a\n"):
    raise SystemExit(f"{path} is not a PNG screenshot")

offset = 8
width = 0
height = 0
bit_depth = 0
color_type = 0
idat = bytearray()
while offset + 8 <= len(data):
    length = struct.unpack(">I", data[offset:offset + 4])[0]
    chunk_type = data[offset + 4:offset + 8]
    chunk_data = data[offset + 8:offset + 8 + length]
    offset += 12 + length
    if chunk_type == b"IHDR":
        width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(">IIBBBBB", chunk_data)
        if compression != 0 or filter_method != 0 or interlace != 0:
            raise SystemExit("unsupported PNG compression/filter/interlace settings")
    elif chunk_type == b"IDAT":
        idat.extend(chunk_data)
    elif chunk_type == b"IEND":
        break

if width <= 0 or height <= 0 or bit_depth != 8 or color_type not in (2, 6):
    raise SystemExit(f"unsupported PNG format width={width} height={height} bit_depth={bit_depth} color_type={color_type}")

channels = 3 if color_type == 2 else 4
row_bytes = width * channels
raw = zlib.decompress(bytes(idat))
rows = []
raw_offset = 0
previous = bytearray(row_bytes)
for _ in range(height):
    filter_type = raw[raw_offset]
    raw_offset += 1
    row = bytearray(raw[raw_offset:raw_offset + row_bytes])
    raw_offset += row_bytes
    for i in range(row_bytes):
        left = row[i - channels] if i >= channels else 0
        up = previous[i]
        upper_left = previous[i - channels] if i >= channels else 0
        if filter_type == 1:
            row[i] = (row[i] + left) & 0xFF
        elif filter_type == 2:
            row[i] = (row[i] + up) & 0xFF
        elif filter_type == 3:
            row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
        elif filter_type == 4:
            row[i] = (row[i] + paeth(left, up, upper_left)) & 0xFF
        elif filter_type != 0:
            raise SystemExit(f"unsupported PNG filter {filter_type}")
    rows.append(row)
    previous = row

step_x = max(1, width // 64)
step_y = max(1, height // 64)
bright_samples = 0
color_samples = 0
total_brightness = 0
sample_count = 0
for y in range(0, height, step_y):
    row = rows[y]
    for x in range(0, width, step_x):
        index = x * channels
        red = row[index]
        green = row[index + 1]
        blue = row[index + 2]
        brightness = max(red, green, blue)
        total_brightness += brightness
        sample_count += 1
        if brightness > 24:
            bright_samples += 1
        if max(red, green, blue) - min(red, green, blue) > 12:
            color_samples += 1

average_brightness = total_brightness / sample_count if sample_count else 0
print(
    f"screenshot={path} width={width} height={height} "
    f"avg_brightness={average_brightness:.1f} bright_samples={bright_samples} "
    f"color_samples={color_samples}"
)

if bright_samples < min_bright_samples:
    raise SystemExit("screenshot appears too dark; possible black-frame regression")
if color_samples < min_color_samples:
    raise SystemExit("screenshot has too little color variation; possible grayscale/preview regression")
PY
