# Contributing

* `frame/` — C, builds with `make`. `src/viewer.c` is the shared pipeline; `src/vr_backend.h` is the SteamVR backend (`-DMD_VR`).
  Menu UI: `src/menu.h`, `src/ui.h`. Discovery: `src/discovery.h`. Crypto: `src/mdcrypto.h`.
* `mac/` — Swift (no Xcode project needed): `./build-app.sh` (`INSTALL=0` to only compile).
* The two crypto implementations must stay byte-identical: run `frame/sideframe --selftest` and `mac-capture selftest` (see `mac/Sources/mac-capture/main.swift`) and compare.
* `frame/tools/uitest.c` renders every menu screen to PPM files so UI changes can be checked without a headset.
* Keep the Frame cool: long test runs heat the headset. Prefer short sessions.
* By contributing you agree your work is released under the MIT license.
