# Sideframe — Frame side.
#   make            builds both programs
#   make install    installs to ~/.local (no root needed):  PREFIX=~/.local make install
#   make uninstall
# sideframe        : the SteamVR dashboard tab (the real app)
# sideframe-window : plain Wayland window viewer (fallback / test tool) — same decode/network/crypto code, different display backend
CC ?= gcc
PREFIX ?= $(HOME)/.local
COMMON = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-missing-braces -I/usr/include/libdrm $(shell pkg-config --cflags libavformat libavcodec libavutil libsodium)
CFLAGS += $(COMMON) $(shell pkg-config --cflags wayland-client)
LDLIBS += $(shell pkg-config --libs wayland-client libavformat libavcodec libavutil libsodium) -lm
SRC = src/viewer.c $(filter-out src/viewer.c,$(wildcard src/*.c))
OPENVR_INC ?= /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
OPENVR_LIB ?= /opt/steamvr/bin/linuxarm64
VR_LIBS = $(shell pkg-config --libs libavformat libavcodec libavutil libsodium) -L$(OPENVR_LIB) -lopenvr_api -Wl,-rpath,$(OPENVR_LIB) -lvulkan -lm

all: sideframe sideframe-window

sideframe-window: $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

src/vert.spv.h: src/shaders/fullscreen.vert
	glslc -mfmt=c -fshader-stage=vert $< -o $@
src/frag.spv.h: src/shaders/ycbcr.frag
	glslc -mfmt=c -fshader-stage=frag $< -o $@

sideframe: src/viewer.c $(wildcard src/*.h) src/vert.spv.h src/frag.spv.h
	$(CC) $(COMMON) $(shell pkg-config --cflags freetype2) -DMD_VR -I$(OPENVR_INC) -Isrc -o $@ src/viewer.c $(VR_LIBS) $(shell pkg-config --libs freetype2) -lpthread

install: all
	install -Dm755 sideframe $(PREFIX)/bin/sideframe
	install -Dm755 sideframe-window $(PREFIX)/bin/sideframe-window
	install -Dm755 tools/launch.sh $(PREFIX)/bin/sideframe-launch
	install -Dm644 assets/fonts/NotoSans-Regular.ttf $(PREFIX)/share/sideframe/fonts/NotoSans-Regular.ttf
	install -Dm644 assets/fonts/NotoSans-Bold.ttf $(PREFIX)/share/sideframe/fonts/NotoSans-Bold.ttf
	install -Dm644 assets/fonts/LICENSE $(PREFIX)/share/sideframe/fonts/LICENSE
	install -Dm644 assets/sideframe-1024.png $(PREFIX)/share/icons/hicolor/1024x1024/apps/sideframe.png
	install -Dm644 assets/sideframe-256.png $(PREFIX)/share/icons/hicolor/256x256/apps/sideframe.png
	sed 's|@BIN@|$(PREFIX)/bin/sideframe-launch|' assets/sideframe.desktop.in | install -Dm644 /dev/stdin $(PREFIX)/share/applications/sideframe.desktop
	@echo "Installed. Find 'Sideframe' in your app list (add it to Steam as a non-Steam game for the Frame's library)."

uninstall:
	rm -f $(PREFIX)/bin/sideframe $(PREFIX)/bin/sideframe-window $(PREFIX)/bin/sideframe-launch $(PREFIX)/share/applications/sideframe.desktop
	rm -rf $(PREFIX)/share/sideframe
	rm -f $(PREFIX)/share/icons/hicolor/1024x1024/apps/sideframe.png $(PREFIX)/share/icons/hicolor/256x256/apps/sideframe.png

clean:
	rm -f sideframe sideframe-window
.PHONY: all install uninstall clean
