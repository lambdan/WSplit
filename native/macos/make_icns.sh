#!/bin/sh
# make_icns.sh ICON_DIR OUT.icns  - builds a macOS .icns from the PNG icons
set -e
SRC="$1"; OUT="$2"
SET="$(mktemp -d)/WSplit.iconset"
mkdir -p "$SET"
cp "$SRC/icon_016.png" "$SET/icon_16x16.png"
cp "$SRC/icon_032.png" "$SET/icon_16x16@2x.png"
cp "$SRC/icon_032.png" "$SET/icon_32x32.png"
sips -z 64 64 "$SRC/icon_128.png" --out "$SET/icon_32x32@2x.png" >/dev/null
cp "$SRC/icon_128.png" "$SET/icon_128x128.png"
cp "$SRC/icon_256.png" "$SET/icon_128x128@2x.png"
cp "$SRC/icon_256.png" "$SET/icon_256x256.png"
cp "$SRC/icon_512.png" "$SET/icon_256x256@2x.png"
cp "$SRC/icon_512.png" "$SET/icon_512x512.png"
iconutil -c icns "$SET" -o "$OUT"
