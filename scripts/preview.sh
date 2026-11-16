#!/usr/bin/env bash
# Render the firmware UI on the host and build tools/preview/out/preview.png.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=tools/preview/build
OUT=tools/preview/out

mkdir -p "$BUILD"
rm -rf "$OUT"
mkdir -p "$OUT"

echo "Compiling host preview ..."
c++ -std=c++17 -O2 -Wall -Wextra \
  -Itools/preview -Ifirmware/src \
  tools/preview/main.cpp firmware/src/ui.cpp firmware/src/morse.cpp \
  -o "$BUILD/preview"

echo "Rendering frames ..."
"$BUILD/preview" "$OUT"

echo "Composing $OUT/preview.png ..."
shopt -s nullglob
frames=("$OUT"/[0-9][0-9]-*.ppm)
shopt -u nullglob

if [[ ${#frames[@]} -eq 0 ]]; then
  echo "no frames rendered" >&2
  exit 1
fi

# Use a font file so the label rendering does not depend on fontconfig names.
LABEL_FONT="$(fc-match -f '%{file}' sans 2>/dev/null || true)"
font_args=()
[[ -f "$LABEL_FONT" ]] && font_args=(-font "$LABEL_FONT")

# 2x nearest-neighbour keeps the pixel grid readable, like the panel.
magick montage \
  -background '#080c18' -fill '#d8e2f4' \
  "${font_args[@]}" -pointsize 16 \
  -label '%t' "${frames[@]}" \
  -filter point -geometry 340x640+10+10 -tile 4x \
  "$OUT/preview.png"

rm -f "$OUT"/*.ppm
echo "Wrote $OUT/preview.png"
