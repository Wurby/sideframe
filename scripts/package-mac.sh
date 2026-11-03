#!/bin/bash
# Builds the Mac release download:  dist/sideframe-mac-VERSION.zip  (run this ON a Mac)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); V=$(cat "$ROOT/VERSION")
(cd "$ROOT/mac" && INSTALL=0 ./build-app.sh >/dev/null)
OUT="$ROOT/dist"; STAGE="$OUT/sideframe-mac-$V"; rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$ROOT/mac/dist/Sideframe.app" "$STAGE/"; cp "$ROOT/mac/install.sh" "$ROOT/LICENSE" "$ROOT/README.md" "$STAGE/"
(cd "$OUT" && ditto -c -k --keepParent "sideframe-mac-$V" "sideframe-mac-$V.zip" && shasum -a 256 "sideframe-mac-$V.zip" > "sideframe-mac-$V.zip.sha256")
echo "built $OUT/sideframe-mac-$V.zip"
