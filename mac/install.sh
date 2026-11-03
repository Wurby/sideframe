#!/bin/bash
# Sideframe installer for the Mac. Installs Sideframe.app into ~/Applications and opens it.
#   ./install.sh              from a release download (uses the Sideframe.app next to this file) or from a source checkout (builds it)
#   ./install.sh --uninstall  remove the app (your pairings in ~/Library/Application Support/Sideframe are kept)
set -euo pipefail
cd "$(dirname "$0")"
DEST="$HOME/Applications/Sideframe.app"
say() { printf '\033[1m%s\033[0m\n' "$*"; }
if [ "${1:-}" = "--uninstall" ]; then
  pkill -x Sideframe 2>/dev/null || true; rm -rf "$DEST"
  say "Sideframe removed. To also forget paired Frames, delete ~/Library/Application Support/Sideframe"; exit 0
fi
[ "$(uname)" = Darwin ] || { echo "This installer is for macOS."; exit 1; }
if [ -d Sideframe.app ]; then SRC=Sideframe.app
else
  command -v swiftc >/dev/null || { echo "Building needs Apple's command-line tools. Run:  xcode-select --install   then run this again."; exit 1; }
  say "Building Sideframe (about a minute)…"; INSTALL=0 ./build-app.sh >/dev/null; SRC=dist/Sideframe.app
fi
pkill -x Sideframe 2>/dev/null || true
mkdir -p "$HOME/Applications"; rm -rf "$DEST"; cp -R "$SRC" "$DEST"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true   # a downloaded copy is flagged by Gatekeeper; this app is ad-hoc signed, not notarized
open "$DEST"
say "Sideframe is installed and running (look for its icon in the menu bar)."
cat <<MSG

Next steps
  1. macOS will ask for two permissions — allow both:
       Screen Recording (so it can show your screen)   and   Accessibility (so the Frame can move the pointer and type).
     If it asks you to quit & reopen the app after Screen Recording, do that.
  2. Click the menu-bar icon → "Pair a Steam Frame…" and keep the 8-digit code visible.
  3. On the Frame, open the Sideframe tab, choose "Add a Mac", pick this Mac and type the code.
MSG
