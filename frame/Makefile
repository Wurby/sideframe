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
	PREFIX=$(PREFIX) ./install.sh

uninstall:
	PREFIX=$(PREFIX) ./install.sh --uninstall

clean:
	rm -f sideframe sideframe-window
.PHONY: all install uninstall clean
