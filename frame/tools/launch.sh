#!/bin/bash
# Sideframe launcher — what the Frame's app entry runs. There is no terminal, so everything is logged to
# ~/.local/state/sideframe/launch.log and problems are shown as a pop-up.
#   * SteamVR running  -> the Sideframe dashboard tab (sideframe): its own menu handles pairing, connecting and closing
#   * SteamVR not running -> a pop-up (and, if a Mac is already paired, the plain desktop window viewer as a fallback)
# Tapping the entry again while Sideframe is running closes it.
BIN=$(dirname "$(readlink -f "$0")")                      # the installed programs (sideframe, sideframe-window) live next to this script
LOGDIR="${XDG_STATE_HOME:-$HOME/.local/state}/sideframe"; mkdir -p "$LOGDIR"
[ "$(stat -c %s "$LOGDIR/launch.log" 2>/dev/null || echo 0)" -gt 1000000 ] && mv -f "$LOGDIR/launch.log" "$LOGDIR/launch.log.old"   # keep the log small
exec >>"$LOGDIR/launch.log" 2>&1
echo; echo "=== $(date '+%F %T') launch pid $$  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-unset}"

SSH_HOST=$(sed -n 's/^ssh_host=//p' "$HOME/.config/sideframe/dev.conf" 2>/dev/null | head -1)   # optional developer convenience
SSH="ssh -o BatchMode=yes -o ConnectTimeout=5 ${SSH_HOST:-none}"

popup() {   # popup error|info "text"
  echo "[$1] $2"
  if command -v kdialog >/dev/null; then
    if [ "$1" = error ]; then kdialog --title "Sideframe" --error "$2" & else kdialog --title "Sideframe" --passivepopup "$2" 8 & fi
  elif command -v notify-send >/dev/null; then notify-send "Sideframe" "$2" & fi
}
find_wayland() {
  local disp=${WAYLAND_DISPLAY:-wayland-0} d
  for d in "${XDG_RUNTIME_DIR:-}" "/run/user/$(id -u)/nested_plasma" "/run/user/$(id -u)"; do
    [ -n "$d" ] && [ -S "$d/$disp" ] && { export XDG_RUNTIME_DIR=$d WAYLAND_DISPLAY=$disp; return 0; }
  done
  return 1
}
find_wayland
trap 'exit 143' TERM INT HUP

# (developer convenience only) make sure the Mac app is running — normal users just keep Sideframe running on their Mac
if [ -n "$SSH_HOST" ] && $SSH true 2>/dev/null; then
  $SSH 'pgrep -x Sideframe >/dev/null' || { echo "starting Sideframe on the Mac over SSH"; $SSH 'open -g ~/Applications/Sideframe.app'; sleep 3; }
fi

t0=$(date +%s)
if [ -x "$BIN/sideframe" ] && pgrep -x vrserver >/dev/null; then
  echo "SteamVR is running: starting the Sideframe tab"
  "$BIN/sideframe" --quiet --toggle
  rc=$?
elif [ -n "$("$BIN/sideframe-window" --list-macs 2>/dev/null | grep -v '^No paired')" ] && [ -n "$WAYLAND_DISPLAY" ]; then
  popup info "SteamVR isn't running, so Sideframe is opening as a desktop window."
  "$BIN/sideframe-window" --quiet
  rc=$?
else
  popup error "SteamVR isn't running. Start SteamVR (put on your headset), then tap Sideframe again."
  exit 1
fi
echo "exited rc=$rc after $(( $(date +%s) - t0 )) s"
[ $rc -ne 0 ] && [ $(( $(date +%s) - t0 )) -lt 10 ] && popup error "Sideframe stopped right away (code $rc). See ~/.local/state/sideframe/launch.log"
exit $rc
