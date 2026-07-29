#!/usr/bin/env bash
# Rasterize assets/retcomm.svg → PNG / ICO (optional ICNS on macOS).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SVG="${ROOT}/assets/retcomm.svg"
PNG="${ROOT}/assets/retcomm.png"
ICO="${ROOT}/assets/retcomm.ico"

if [[ ! -f "${SVG}" ]]; then
  echo "missing ${SVG}" >&2
  exit 1
fi

if command -v rsvg-convert >/dev/null 2>&1; then
  rsvg-convert -w 1024 -h 1024 "${SVG}" -o "${PNG}"
elif command -v magick >/dev/null 2>&1; then
  magick -background none "${SVG}" -resize 1024x1024 "${PNG}"
elif command -v convert >/dev/null 2>&1; then
  convert -background none "${SVG}" -resize 1024x1024 "${PNG}"
elif command -v inkscape >/dev/null 2>&1; then
  inkscape "${SVG}" -w 1024 -h 1024 -o "${PNG}"
else
  # Pure-Python fallback (no Cairo): draw a matching placeholder PNG.
  python3 - "${PNG}" <<'PY'
import struct, zlib, sys
from pathlib import Path

def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

out = Path(sys.argv[1])
w = h = 256
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        cx, cy = x - 128, y - 128
        inside = abs(cx) < 100 and abs(cy) < 80
        edge = abs(cx) < 105 and abs(cy) < 85
        if edge and not inside:
            r, g, b, a = 45, 212, 191, 255
        elif inside and (abs(x - 90) < 10 and abs(y - 128) < 10):
            r, g, b, a = 45, 212, 191, 255
        elif inside and 110 <= x <= 170 and 108 <= y <= 120:
            r, g, b, a = 45, 212, 191, 255
        elif inside and 110 <= x <= 150 and 132 <= y <= 144:
            r, g, b, a = 45, 212, 191, 180
        elif cx * cx + cy * cy < 120 * 120:
            t = (x + y) / 512
            r = int(26 + t * 10)
            g = int(35 + t * 10)
            b = int(50 + t * 10)
            a = 255
        else:
            r = g = b = a = 0
        row.extend([r, g, b, a])
    rows.append(bytes(row))
raw = b"".join(rows)
ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
out.write_bytes(png)
print(f"wrote {out} (python fallback)")
PY
fi

echo "PNG: ${PNG}"

if command -v magick >/dev/null 2>&1; then
  magick "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
elif command -v convert >/dev/null 2>&1; then
  convert "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
fi
