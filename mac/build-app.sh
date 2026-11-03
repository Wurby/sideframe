#!/bin/bash
# Builds Sideframe.app (menu-bar app) from the shared sources + App/AppMain.swift.
#   ./build-app.sh              build into dist/ and install to ~/Applications
#   INSTALL=0 ./build-app.sh    build into dist/ only (compile check)
# The app is ad-hoc signed (no Apple developer account needed). macOS ties the Screen Recording / Accessibility permissions to the exact
# build, so after every rebuild that you install, the first launch asks again. (A Developer ID signature would keep them across builds.)
set -euo pipefail
cd "$(dirname "$0")"
APP=dist/Sideframe.app
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
SRC=Sources/mac-capture
swiftc -O -parse-as-library -o "$APP/Contents/MacOS/Sideframe" \
    App/AppMain.swift $SRC/Config.swift $SRC/Stats.swift $SRC/Crypto.swift $SRC/KeyStore.swift $SRC/Pairing.swift $SRC/Input.swift $SRC/Server.swift $SRC/Capture.swift
cp App/Info.plist "$APP/Contents/Info.plist"
if [ -f App/AppIcon-1024.png ]; then
    ICONSET=$(mktemp -d)/AppIcon.iconset; mkdir -p "$ICONSET"
    for s in 16 32 128 256 512; do
        sips -z $s $s App/AppIcon-1024.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
        sips -z $((s*2)) $((s*2)) App/AppIcon-1024.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" || echo "icon build skipped"
fi
codesign --force --deep --sign - --identifier org.sideframe.Sideframe "$APP"
echo "built $APP"
if [ "${INSTALL:-1}" = 1 ]; then
    mkdir -p "$HOME/Applications"
    rm -rf "$HOME/Applications/Sideframe.app"; cp -R "$APP" "$HOME/Applications/Sideframe.app"
    echo "installed: $HOME/Applications/Sideframe.app"
fi
codesign -dv "$APP" 2>&1 | grep -E "Identifier|Signature|CDHash" || true
