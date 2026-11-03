#!/bin/bash
# Builds the Frame release download:  dist/sideframe-frame-VERSION-aarch64.tar.gz  (run this ON a Steam Frame)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); V=$(cat "$ROOT/VERSION")
[ "$(uname -m)" = aarch64 ] || { echo "Build the Frame release on a Steam Frame (aarch64)."; exit 1; }
(cd "$ROOT/frame" && make -B all)
OUT="$ROOT/dist"; STAGE="$OUT/sideframe-frame-$V"; rm -rf "$STAGE"; mkdir -p "$STAGE/assets/fonts"
cd "$ROOT/frame"
cp sideframe sideframe-window install.sh "$STAGE/"; cp tools/launch.sh "$STAGE/launch.sh"
cp assets/sideframe-1024.png assets/sideframe-256.png assets/sideframe.desktop.in "$STAGE/assets/"
cp assets/fonts/NotoSans-Regular.ttf assets/fonts/NotoSans-Bold.ttf assets/fonts/LICENSE "$STAGE/assets/fonts/"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/SECURITY.md" "$STAGE/"
strip "$STAGE/sideframe" "$STAGE/sideframe-window" 2>/dev/null || true
tar -C "$OUT" -czf "$STAGE-aarch64.tar.gz" "sideframe-frame-$V"
(cd "$OUT" && sha256sum "sideframe-frame-$V-aarch64.tar.gz" > "sideframe-frame-$V-aarch64.tar.gz.sha256")
echo "built $STAGE-aarch64.tar.gz"
