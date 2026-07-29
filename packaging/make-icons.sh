#!/usr/bin/env bash
# Rasterize assets/retcomm.svg → PNG / ICO.
# Master PNG is 512x512 — linuxdeploy rejects 1024 (and other non-hicolor sizes).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SVG="${ROOT}/assets/retcomm.svg"
PNG="${ROOT}/assets/retcomm.png"
ICO="${ROOT}/assets/retcomm.ico"
SIZE=512

if [[ ! -f "${SVG}" ]]; then
  echo "missing ${SVG}" >&2
  exit 1
fi

if command -v rsvg-convert >/dev/null 2>&1; then
  rsvg-convert -w "${SIZE}" -h "${SIZE}" "${SVG}" -o "${PNG}"
elif command -v magick >/dev/null 2>&1; then
  magick -background none "${SVG}" -resize "${SIZE}x${SIZE}" "${PNG}"
elif command -v convert >/dev/null 2>&1; then
  convert -background none "${SVG}" -resize "${SIZE}x${SIZE}" "${PNG}"
elif command -v inkscape >/dev/null 2>&1; then
  inkscape "${SVG}" -w "${SIZE}" -h "${SIZE}" -o "${PNG}"
else
  # Pure-Python fallback (no Cairo): draw a matching placeholder PNG at 512.
  python3 - "${PNG}" "${SIZE}" <<'PY'
import struct, zlib, sys
from pathlib import Path

def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

out = Path(sys.argv[1])
w = h = int(sys.argv[2])
half = w // 2
scale = w / 512.0
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        sx, sy = x / scale, y / scale
        scx, scy = sx - 256, sy - 256
        inside = abs(scx) < 100 and abs(scy) < 80
        edge = abs(scx) < 105 and abs(scy) < 85
        if edge and not inside:
            r, g, b, a = 45, 212, 191, 255
        elif inside and (abs(sx - 176) < 28 and abs(sy - 256) < 28):
            r, g, b, a = 45, 212, 191, 255
        elif inside and 240 <= sx <= 360 and 220 <= sy <= 244:
            r, g, b, a = 45, 212, 191, 255
        elif inside and 240 <= sx <= 320 and 268 <= sy <= 292:
            r, g, b, a = 45, 212, 191, 180
        elif scx * scx + scy * scy < 220 * 220:
            t = (sx + sy) / 1024
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

echo "PNG: ${PNG} (${SIZE}x${SIZE})"

if command -v magick >/dev/null 2>&1; then
  magick "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
elif command -v convert >/dev/null 2>&1; then
  convert "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
fi
