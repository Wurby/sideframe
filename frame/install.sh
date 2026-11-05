#!/bin/bash
# Sideframe installer for the Steam Frame. No root needed: installs into ~/.local.
#   ./install.sh              install (from a release download, or from a source checkout — builds it if needed)
#   ./install.sh --uninstall  remove everything it installed (your pairings in ~/.config/sideframe are kept)
#   PREFIX=/some/dir ./install.sh   install somewhere else
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
PREFIX=${PREFIX:-$HOME/.local}
say()  { printf '\033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m! %s\033[0m\n' "$*"; }
die()  { printf '\033[31mx %s\033[0m\n' "$*" >&2; exit 1; }

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$PREFIX/bin/sideframe" "$PREFIX/bin/sideframe-window" "$PREFIX/bin/sideframe-launch" "$PREFIX/share/applications/sideframe.desktop"
  rm -rf "$PREFIX/share/sideframe" "$HOME/.local/share/sideframe"
  rm -f "$PREFIX/share/icons/hicolor/1024x1024/apps/sideframe.png" "$PREFIX/share/icons/hicolor/256x256/apps/sideframe.png"
  say "Sideframe removed. (Pairings in ~/.config/sideframe were kept; delete that folder to forget your Macs.)"
  exit 0
fi

say "Installing Sideframe for the Steam Frame"
[ "$(uname -m)" = aarch64 ] || warn "This isn't an aarch64 machine ($(uname -m)). Sideframe is made for the Steam Frame."
[ -d /opt/steamvr ] || warn "SteamVR wasn't found in /opt/steamvr. Sideframe needs SteamVR to show its tab."

# 1. the programs: use the prebuilt ones from a release download, otherwise build from source
if [ ! -x ./sideframe ]; then
  [ -f Makefile ] || die "No programs and no source here. Download the release from the Sideframe page and run install.sh from the unpacked folder."
  say "Building from source (this takes about a minute)…"
  for t in gcc make pkg-config glslc; do command -v "$t" >/dev/null || die "Missing build tool: $t (the README lists what to install)."; done
  env -u LD_LIBRARY_PATH make
fi
LAUNCH=launch.sh; [ -f "$LAUNCH" ] || LAUNCH=tools/launch.sh
[ -f "$LAUNCH" ] || die "launch.sh is missing from this download."
DESKTOP=assets/sideframe.desktop.in
[ -f "$DESKTOP" ] || die "assets/sideframe.desktop.in is missing from this download."

# 2. libraries it needs (all ship with SteamOS; this just gives a clear message if one is missing)
missing=$(ldd ./sideframe 2>&1 | awk '/not found/{print $1}' | sort -u | tr '\n' ' ')
[ -z "$missing" ] || die "Missing system libraries: $missing"

# 3. install
install -Dm755 sideframe        "$PREFIX/bin/sideframe"
install -Dm755 sideframe-window "$PREFIX/bin/sideframe-window"
install -Dm755 "$LAUNCH"        "$PREFIX/bin/sideframe-launch"
for f in NotoSans-Regular.ttf NotoSans-Bold.ttf LICENSE; do install -Dm644 "assets/fonts/$f" "$PREFIX/share/sideframe/fonts/$f"; done
install -Dm644 assets/sideframe-1024.png "$PREFIX/share/icons/hicolor/1024x1024/apps/sideframe.png"
install -Dm644 assets/sideframe-256.png  "$PREFIX/share/icons/hicolor/256x256/apps/sideframe.png"
sed "s|@BIN@|$PREFIX/bin/sideframe-launch|" "$DESKTOP" | install -Dm644 /dev/stdin "$PREFIX/share/applications/sideframe.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true

say "Done."
cat <<MSG

Next steps
  1. On your Mac: install and open Sideframe, then choose "Pair a Steam Frame…" from its menu-bar icon (it shows an 8-digit code).
  2. On the Frame: start SteamVR, open the dashboard and launch "Sideframe" (from the app list, or add it to Steam as a non-Steam game).
  3. In the Sideframe tab choose "Add a Mac", pick your Mac and type the code.

To remove Sideframe later:  ./install.sh --uninstall
MSG
