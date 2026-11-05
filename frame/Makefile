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
COMMON = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-missing-braces $(FFMPEG_DEF) -I/usr/include/libdrm $(shell pkg-config --cflags $(FFMPEG_PKGS) libsodium)
CFLAGS += $(COMMON) $(shell pkg-config --cflags wayland-client)
LDLIBS += $(shell pkg-config --libs wayland-client $(FFMPEG_PKGS) libsodium) -lm
SRC = src/viewer.c $(filter-out src/viewer.c,$(wildcard src/*.c))
SIDEFRAME_VERSION ?= $(shell cat ../VERSION 2>/dev/null || echo dev)
SIDEFRAME_UPDATE_URL ?=   # https URL of a text file containing the latest version number; empty disables the update-check button
SIDEFRAME_URL ?=   # shown in the app's "install the Mac app" hints, e.g. make SIDEFRAME_URL=github.com/you/sideframe
OPENVR_INC ?= /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
OPENVR_LIB ?= /opt/steamvr/bin/linuxarm64
VR_LIBS = $(shell pkg-config --libs $(FFMPEG_PKGS) libsodium) -L$(OPENVR_LIB) -lopenvr_api -Wl,-rpath,$(OPENVR_LIB) -lvulkan -lm

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
