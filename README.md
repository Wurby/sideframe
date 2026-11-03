# Sideframe

Your Mac's desktop, in your Steam Frame — as a tab in the SteamVR dashboard.

Sideframe mirrors your Mac's screen to the headset with low latency, hardware video decoding and encrypted transport, and forwards your
pointer, a physical/Bluetooth keyboard, and the SteamVR keyboard back to the Mac.

> Sideframe is an independent open-source project. It is not affiliated with or endorsed by Apple or Valve.
> "Mac" and "Steam Frame" are used only to describe what it works with.

## How it works

```
Mac:  ScreenCaptureKit → VideoToolbox HEVC → encrypted UDP ─────────────┐
Frame: UDP → loss repair → V4L2 hardware decode → dmabuf → Vulkan → SteamVR dashboard tab
                    pointer / keyboard ← encrypted UDP ← overlay events + evdev keyboards
```

* No CPU copies of video on the Frame (decoder buffers are imported straight into Vulkan).
* Everything on the wire is ChaCha20-Poly1305 encrypted with a per-pair key; see [docs/PROTOCOL.md](docs/PROTOCOL.md) and [SECURITY.md](SECURITY.md).
* The picture is sized from the headset's resolution, not hard-coded, so any Mac display works.

## Requirements

* **Mac:** macOS 13+ (ScreenCaptureKit), Xcode command-line tools (`swiftc`). Same local network as the Frame.
* **Frame:** SteamOS on the Steam Frame with SteamVR installed. Build tools: `gcc`, `make`, `pkg-config`, `glslc`, and dev packages for
  `libavformat/libavcodec/libavutil` (ffmpeg), `libsodium`, `freetype2`, `vulkan`, `wayland-client`. The OpenVR headers ship with SteamVR
  (`/opt/steamvr/tools/hellovr_vulkan_linux/...`); override with `OPENVR_INC=` / `OPENVR_LIB=`.

## Install

**Mac**

```sh
cd mac && ./build-app.sh        # builds and installs ~/Applications/Sideframe.app (ad-hoc signed)
open ~/Applications/Sideframe.app
```

Grant **Screen Recording** (to see the screen) and **Accessibility** (so the Frame can move the pointer and type) when asked.
The app is ad-hoc signed, so macOS asks again after each rebuild you install.

**Frame**

```sh
cd frame && make && make install     # installs to ~/.local (no root)
```

Then find **Sideframe** in the app list. To launch it from the Frame's library, add it as a non-Steam game.

## Pairing (no SSH, no config files)

1. On the Mac, choose **Pair a Steam Frame…** in the Sideframe menu-bar icon. It shows an 8-digit code (it only changes when you reopen the
   window or click *New Code*).
2. On the Frame, open the Sideframe tab in the SteamVR dashboard → **Add a Mac** → pick your Mac → type the code.
3. Press **Connect**. Close the tab with the **Close** button under it in the dashboard.

## Using it

* Point the laser at the tab to move the Mac's pointer; click, right-click and scroll work.
* The keyboard button under the tab opens the SteamVR keyboard. A physical or Bluetooth keyboard types into the Mac while the laser is on the tab.
* `sideframe --help`-style options: `--scale F` (force a fixed fraction of the Mac's resolution), `--max-fps N`, `--idle-exit SECONDS`.

## Known limits

* One Mac display (the main one) is mirrored; no virtual displays.
* Not an on-screen keyboard replacement for third-party keyboard apps yet (they type into the compositor focus, not overlays).
* The Mac app is ad-hoc signed; a Developer ID build would keep permissions across updates.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bugs and pull requests welcome.

## License

MIT — see [LICENSE](LICENSE). Bundled font: Noto Sans (SIL OFL, see `frame/assets/fonts/LICENSE`).
