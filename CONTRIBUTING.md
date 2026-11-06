# Contributing

* `frame/` — C, builds with `make`. `src/viewer.c` is the shared pipeline; `src/vr_backend.h` is the SteamVR backend (`-DMD_VR`).
  Menu UI: `src/menu.h`, `src/ui.h`. Discovery: `src/discovery.h`. Crypto: `src/mdcrypto.h`.
* `mac/` — Swift (no Xcode project needed): `./build-app.sh` (`INSTALL=0` to only compile).
* The two crypto implementations must stay byte-identical: run `frame/sideframe --selftest` and `mac-capture selftest` (see `mac/Sources/mac-capture/main.swift`) and compare.
* `frame/tools/uitest.c` renders every menu screen to PPM files so UI changes can be checked without a headset.
* Keep the Frame cool: long test runs heat the headset. Prefer short sessions.
* By contributing you agree your work is released under the MIT license.

## Keeping the two sides compatible

The Frame and Mac apps are updated separately, so the wire protocol (docs/PROTOCOL.md, currently v1) must stay backward compatible:
* Only add new message types; receivers ignore types they don't know. Never change the layout of an existing message.
* If a break is ever unavoidable, add a version exchange first (a new inner message carrying the protocol version) so each side can tell
  the user which app to update, and ship that exchange in a release before the breaking change.

## Releasing

1. Put the new version in `VERSION` and `latest.txt` (the Frame's update check reads `latest.txt` from `main`).
2. Commit, then `git tag vX.Y.Z && git push origin main vX.Y.Z`. The `build` workflow builds the Frame tarball (arm64) and the Mac zip and attaches both to the release.
