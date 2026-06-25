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

if command -v magick >/dev/null 2>&1; then
  IM=(magick)
elif command -v convert >/dev/null 2>&1; then
  IM=(convert)
else
  echo "error: ImageMagick is required (magick or convert)" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

# Normalize to a square PNG while preserving transparency. The source is already
# the canonical artwork; this just creates packaging-friendly derivatives.
"${IM[@]}" "$SRC" \
  -background none \
  -resize "${SIZE}x${SIZE}" \
  -gravity center \
  -extent "${SIZE}x${SIZE}" \
  "$OUT_DIR/icon.png"
echo "wrote src/icons/icon.png"

# Windows .ico (multi-resolution).
"${IM[@]}" "$OUT_DIR/icon.png" -define icon:auto-resize=256,128,64,48,32,16 "$OUT_DIR/icon.ico"
echo "wrote src/icons/icon.ico"

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
