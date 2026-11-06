# Sideframe — Frame side.
#   make            builds both programs
#   make install    installs to ~/.local (no root needed):  PREFIX=~/.local make install
#   make uninstall
# sideframe        : the SteamVR dashboard tab (the real app)
# sideframe-window : plain Wayland window viewer (fallback / test tool) — same decode/network/crypto code, different display backend
CC ?= gcc
PREFIX ?= $(HOME)/.local
# ffmpeg is OPTIONAL (only to play a raw .h265 test file): make HAVE_FFMPEG=1. Without it the programs link no ffmpeg, so SteamOS ffmpeg updates cannot break them.
FFMPEG_PKGS = $(if $(HAVE_FFMPEG),libavformat libavcodec libavutil,)
FFMPEG_DEF = $(if $(HAVE_FFMPEG),-DHAVE_FFMPEG,)
COMMON = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-missing-braces $(FFMPEG_DEF) -I/usr/include/libdrm $(shell pkg-config --cflags $(FFMPEG_PKGS) libsodium libdrm)
# STATIC_SODIUM=1 links libsodium statically (used for the release build so it doesn't depend on the system's libsodium version)
SODIUM_STATIC := -Wl,-Bstatic -lsodium -Wl,-Bdynamic
SODIUM_LIBS = $(if $(STATIC_SODIUM),$(SODIUM_STATIC),$(shell pkg-config --libs libsodium))
CFLAGS += $(COMMON) $(shell pkg-config --cflags wayland-client)
LDLIBS += $(shell pkg-config --libs wayland-client $(FFMPEG_PKGS)) $(SODIUM_LIBS) -lm
SRC = src/viewer.c $(filter-out src/viewer.c,$(wildcard src/*.c))
SIDEFRAME_VERSION ?= $(shell cat ../VERSION 2>/dev/null || echo dev)
# https URL of a text file containing the latest version number (forks: point this at your own); empty disables the update-check button
SIDEFRAME_UPDATE_URL ?= https://raw.githubusercontent.com/Wurby/sideframe/main/latest.txt
# shown in the app's "install the Mac app" hints, e.g. make SIDEFRAME_URL=github.com/you/sideframe
SIDEFRAME_URL ?= github.com/Wurby/sideframe
# vendored header (BSD-3, see third_party/openvr/LICENSE); the library itself is loaded at run time
OPENVR_INC ?= third_party/openvr
VR_LIBS = $(if $(FFMPEG_PKGS),$(shell pkg-config --libs $(FFMPEG_PKGS)),) $(SODIUM_LIBS) -lvulkan -ldl -lm

all: sideframe sideframe-window

sideframe-window: $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

src/vert.spv.h: src/shaders/fullscreen.vert
	glslc -mfmt=c -fshader-stage=vert $< -o $@
src/frag.spv.h: src/shaders/ycbcr.frag
	glslc -mfmt=c -fshader-stage=frag $< -o $@

sideframe: src/viewer.c $(wildcard src/*.h) src/vert.spv.h src/frag.spv.h
	$(CC) $(COMMON) $(shell pkg-config --cflags freetype2) -DMD_VR -DSIDEFRAME_URL=\"$(SIDEFRAME_URL)\" -DSIDEFRAME_VERSION=\"$(SIDEFRAME_VERSION)\" -DSIDEFRAME_UPDATE_URL=\"$(SIDEFRAME_UPDATE_URL)\" -I$(OPENVR_INC) -Isrc -o $@ src/viewer.c $(VR_LIBS) $(shell pkg-config --libs freetype2) -lpthread

install: all
	PREFIX=$(PREFIX) ./install.sh

uninstall:
	PREFIX=$(PREFIX) ./install.sh --uninstall

clean:
	rm -f sideframe sideframe-window
.PHONY: all install uninstall clean
