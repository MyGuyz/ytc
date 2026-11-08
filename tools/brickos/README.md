# Brick OS build of YTC

Thai support and the TrimUI Brick (tg5040) build used by Brick OS
(https://github.com/MyGuyz/brick-os) as its YouTube app.

- Thai UI (`data/lang/th.json`), Thai font with pre-placed marks (`data/fonts/BrickOSThai.ttf`,
  IBM Plex Sans Thai, OFL; mark placement in `src/text_thai.c`), Kedmanee on-screen keyboard
  (the ก/A key switches layouts).
- `fetch-deps.sh` + `build-deps.sh` cross-build curl/mbedTLS, libass, FFmpeg 6.1 and libmpv 0.36
  in the NX Redux tg5040 toolchain image; `build-ytc.sh` builds YTC and assembles `dist/ytc`.
- `thai_patch.py` is how the Thai changes were first applied to upstream.
