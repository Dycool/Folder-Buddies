#!/usr/bin/env bash
# Derive all platform-specific app icons from the canonical icon.png.
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

# Fill the canvas so the folder artwork extends to the icon corner.
mkdir -p "$OUT_DIR"
DPI=72

if [ ${#IM[@]} -gt 0 ]; then
  "${IM[@]}" "$SRC" \
    -background white \
    -trim -fuzz 2% \
    -resize "${SIZE}x${SIZE}^" \
    -gravity center \
    -extent "${SIZE}x${SIZE}" \
    -density "${DPI}x${DPI}" \
    "$OUT_DIR/icon.png"
  echo "wrote src/icons/icon.png (ImageMagick)"
elif python3 -c 'from PIL import Image; print("ok")' 2>/dev/null; then
  python3 << PY
import sys
from PIL import Image

src = '$SRC'
dst = '$OUT_DIR/icon.png'
size = $SIZE

img = Image.open(src).convert('RGBA')
fuzz = int(255 * 0.02)
bbox = img.getbbox()
if bbox:
    img = img.crop(bbox)
# Scale to fill most of canvas (no cropping) so artwork sits on a full white
# background — like VS Code's solid-colour icon.
fill = 900
ratio = min(fill / img.width, fill / img.height)
new_w = int(round(img.width * ratio))
new_h = int(round(img.height * ratio))
img = img.resize((new_w, new_h), Image.LANCZOS)

# Full white square; macOS applies the rounded-rect mask.
canvas = Image.new('RGBA', (size, size), (255,255,255,255))
x = (size - img.width) // 2
y = (size - img.height) // 2
canvas.paste(img, (x, y), img)
canvas.save(dst, dpi=(72,72))
print(f'wrote src/icons/icon.png (Pillow, {img.width}x{img.height})')
PY
else
  # Fallback: just copy as-is
  cp "$SRC" "$OUT_DIR/icon.png"
  echo "wrote src/icons/icon.png (copy)"
fi

# Windows .ico (multi-resolution).
if [ ${#IM[@]} -gt 0 ]; then
  "${IM[@]}" "$OUT_DIR/icon.png" -define icon:auto-resize=256,128,64,48,32,16 "$OUT_DIR/icon.ico"
  echo "wrote src/icons/icon.ico"
elif python3 -c 'from PIL import Image' 2>/dev/null; then
  python3 << PY
import sys
from PIL import Image
img = Image.open('$OUT_DIR/icon.png').convert('RGBA')
sizes = [(256,256), (128,128), (64,64), (48,48), (32,32), (16,16)]
frames = []
for s in sizes:
    frames.append(img.resize(s, Image.LANCZOS))
frames[0].save('$OUT_DIR/icon.ico', format='ICO', append_images=frames[1:])
print('wrote src/icons/icon.ico (Pillow)')
PY
else
  echo "note: could not write src/icons/icon.ico (need ImageMagick or Pillow)"
fi

# macOS .icns.
if command -v iconutil >/dev/null 2>&1 && command -v sips >/dev/null 2>&1; then
  rm -rf "$OUT_DIR/icon.iconset"
  mkdir "$OUT_DIR/icon.iconset"
  for s in 16 32 128 256 512; do
    sips -z "$s" "$s" "$OUT_DIR/icon.png" --out "$OUT_DIR/icon.iconset/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z "$d" "$d" "$OUT_DIR/icon.png" --out "$OUT_DIR/icon.iconset/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$OUT_DIR/icon.iconset" -o "$OUT_DIR/icon.icns"
  rm -rf "$OUT_DIR/icon.iconset"
  echo "wrote src/icons/icon.icns (iconutil)"
elif python3 - "$OUT_DIR/icon.png" "$OUT_DIR/icon.icns" <<'PY'
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
