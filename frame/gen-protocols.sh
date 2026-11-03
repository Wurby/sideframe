#!/bin/sh
# Regenerate Wayland protocol glue (requires wayland-scanner + wayland-protocols)
P=/usr/share/wayland-protocols
cd "$(dirname "$0")"
for x in stable/xdg-shell/xdg-shell.xml:xdg-shell stable/linux-dmabuf/linux-dmabuf-v1.xml:linux-dmabuf-v1 \
         stable/viewporter/viewporter.xml:viewporter unstable/xdg-decoration/xdg-decoration-unstable-v1.xml:xdg-decoration-unstable-v1 \
         staging/cursor-shape/cursor-shape-v1.xml:cursor-shape-v1 stable/tablet/tablet-v2.xml:tablet-v2; do
  f=${x%%:*}; n=${x##*:}
  wayland-scanner client-header $P/$f src/$n-client-protocol.h
  wayland-scanner private-code  $P/$f src/$n-protocol.c
done
