#!/usr/bin/env bash
# Derive all platform-specific app icons from the canonical icon.png.
# The canonical icon.png is transparent and is never overwritten by this
# script. The white background that macOS needs is built into a temp file.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CANONICAL_ICON="$SCRIPT_DIR/icon.png"
SRC="${1:-$CANONICAL_ICON}"
OUT_DIR="$SCRIPT_DIR"
SIZE=1024

if [ ! -f "$SRC" ]; then
  echo "error: source icon not found: $SRC" >&2
  echo "Put the canonical app icon at: $CANONICAL_ICON" >&2
  exit 1
fi

IM=()
if command -v magick >/dev/null 2>&1; then
  IM=(magick)
elif command -v convert >/dev/null 2>&1; then
  IM=(convert)
fi
# Fall back to Python/Pillow if ImageMagick is unavailable.

mkdir -p "$OUT_DIR"
DPI=72

# ── Windows .ico — from the original source (no white background) ──
if [ ${#IM[@]} -gt 0 ]; then
  "${IM[@]}" "$SRC" -define icon:auto-resize=256,128,64,48,32,16 "$OUT_DIR/icon.ico"
  echo "wrote src/icons/icon.ico (ImageMagick)"
elif python3 -c 'from PIL import Image' 2>/dev/null; then
  python3 << PY
from PIL import Image
img = Image.open('$SRC').convert('RGBA')
sizes = [(256,256), (128,128), (64,64), (48,48), (32,32), (16,16)]
frames = [img.resize(s, Image.LANCZOS) for s in sizes]
frames[0].save('$OUT_DIR/icon.ico', format='ICO', append_images=frames[1:])
print('wrote src/icons/icon.ico (Pillow)')
PY
else
  echo "note: could not write src/icons/icon.ico (need ImageMagick or Pillow)"
fi

# ── macOS .icns — needs white background ──
# Build a white-canvas version in a temp file, then generate .icns from it.
WHITE_PNG="$(mktemp /tmp/icon-white-XXXXX.png)"
trap 'rm -f "$WHITE_PNG"' EXIT

if [ ${#IM[@]} -gt 0 ]; then
  "${IM[@]}" "$SRC" \
    -background white \
    -trim -fuzz 2% \
    -resize "${SIZE}x${SIZE}^" \
    -gravity center \
    -extent "${SIZE}x${SIZE}" \
    -density "${DPI}x${DPI}" \
    "$WHITE_PNG"
elif python3 -c 'from PIL import Image; print("ok")' 2>/dev/null; then
  python3 << PY
from PIL import Image

src = '$SRC'
dst = '$WHITE_PNG'
size = $SIZE

img = Image.open(src).convert('RGBA')
bbox = img.getbbox()
if bbox:
    img = img.crop(bbox)
fill = 900
ratio = min(fill / img.width, fill / img.height)
new_w = int(round(img.width * ratio))
new_h = int(round(img.height * ratio))
img = img.resize((new_w, new_h), Image.LANCZOS)
canvas = Image.new('RGBA', (size, size), (255,255,255,255))
x = (size - img.width) // 2
y = (size - img.height) // 2
canvas.paste(img, (x, y), img)
canvas.save(dst, dpi=(72,72))
print(f'wrote {dst} (Pillow, {img.width}x{img.height})')
PY
else
  cp "$SRC" "$WHITE_PNG"
fi

if command -v iconutil >/dev/null 2>&1 && command -v sips >/dev/null 2>&1; then
  rm -rf "$OUT_DIR/icon.iconset"
  mkdir "$OUT_DIR/icon.iconset"
  for s in 16 32 128 256 512; do
    sips -z "$s" "$s" "$WHITE_PNG" --out "$OUT_DIR/icon.iconset/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z "$d" "$d" "$WHITE_PNG" --out "$OUT_DIR/icon.iconset/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$OUT_DIR/icon.iconset" -o "$OUT_DIR/icon.icns"
  rm -rf "$OUT_DIR/icon.iconset"
  echo "wrote src/icons/icon.icns (iconutil)"
elif python3 - "$WHITE_PNG" "$OUT_DIR/icon.icns" <<'PY'
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
img = Image.open(src).convert("RGBA")
img.save(dst, format="ICNS", sizes=[(16,16),(32,32),(64,64),(128,128),(256,256),(512,512),(1024,1024)])
PY
then
  echo "wrote src/icons/icon.icns (Pillow)"
else
  echo "note: could not write src/icons/icon.icns (need iconutil or python3 with Pillow)"
fi
