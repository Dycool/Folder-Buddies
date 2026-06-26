#!/usr/bin/env bash
# Derive all platform-specific app icons from the canonical icon.png.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CANONICAL_ICON="$SCRIPT_DIR/icon.png"
SRC="${1:-$CANONICAL_ICON}"
OUT_DIR="$SCRIPT_DIR"
SIZE=1024
FILL=900
ICON_SOURCE="$SCRIPT_DIR/icon-source.png"
[ -f "$ICON_SOURCE" ] || ICON_SOURCE="$SRC"

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
mkdir -p "$OUT_DIR"

# macOS/Linux/in-app icon.png — white rounded squircle with a transparent
# margin (macOS Big Sur style). Pillow only, so local previews match CI.
python3 - "$ICON_SOURCE" "$OUT_DIR/icon.png" "$SIZE" <<'PY'
import sys
from PIL import Image, ImageDraw
src_path, dst, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
margin = round(size * 100 / 1024)
folder_fill = 0.62
radius_frac = 0.225
img = Image.open(src_path).convert('RGBA')
bbox = img.getbbox()
if bbox:
    img = img.crop(bbox)
body = size - 2 * margin
radius = int(body * radius_frac)
canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))
ImageDraw.Draw(canvas).rounded_rectangle(
    [margin, margin, margin + body, margin + body], radius=radius,
    fill=(255, 255, 255, 255))
fw = int(size * folder_fill)
ratio = fw / img.width
folder = img.resize((fw, int(img.height * ratio)), Image.LANCZOS)
canvas.alpha_composite(folder, ((size - fw) // 2, (size - folder.height) // 2))
canvas.save(dst, dpi=(72, 72))
print('wrote src/icons/icon.png (squircle)')
PY

# Windows .ico (multi-resolution, transparent background).
if [ ${#IM[@]} -gt 0 ]; then
  "${IM[@]}" "$ICON_SOURCE" \
    -background none \
    -trim -fuzz 2% \
    -resize "${FILL}x${FILL}" \
    -gravity center \
    -extent "${SIZE}x${SIZE}" \
    -define icon:auto-resize=256,128,64,48,32,16 \
    "$OUT_DIR/icon.ico"
  echo "wrote src/icons/icon.ico (transparent)"
elif python3 -c 'from PIL import Image' 2>/dev/null; then
  python3 << PY
from PIL import Image
img = Image.open('$ICON_SOURCE').convert('RGBA')
bbox = img.getbbox()
if bbox:
    img = img.crop(bbox)
ratio = min($FILL / img.width, $FILL / img.height)
img = img.resize((round(img.width * ratio), round(img.height * ratio)), Image.LANCZOS)
canvas = Image.new('RGBA', ($SIZE, $SIZE), (0, 0, 0, 0))
canvas.paste(img, (($SIZE - img.width) // 2, ($SIZE - img.height) // 2), img)
sizes = [(256,256), (128,128), (64,64), (48,48), (32,32), (16,16)]
frames = [canvas.resize(s, Image.LANCZOS) for s in sizes]
frames[0].save('$OUT_DIR/icon.ico', format='ICO', append_images=frames[1:])
print('wrote src/icons/icon.ico (Pillow, transparent)')
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
