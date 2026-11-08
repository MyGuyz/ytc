#!/bin/bash
# Download (on the WSL host) the sources the YTC media/TLS stack is cross-built from.
set -euo pipefail
S=~/ytc-deps/src; mkdir -p $S; cd $S
get() { [ -s "$2" ] || wget -q --tries=5 --timeout=60 -O "$2" "$1"; echo "ok $2"; }
get https://zlib.net/fossils/zlib-1.3.1.tar.gz zlib.tar.gz
get https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 mbedtls.tar.bz2
# the toolchain image (and this host) have no bzip2: recompress with Python's bz2 module
[ -s mbedtls.tar.gz ] || python3 -c "import bz2,gzip,shutil; shutil.copyfileobj(bz2.open('mbedtls.tar.bz2'), gzip.open('mbedtls.tar.gz','wb'))"
get https://curl.se/download/curl-8.11.0.tar.gz curl.tar.gz
get https://github.com/fribidi/fribidi/releases/download/v1.0.13/fribidi-1.0.13.tar.xz fribidi.tar.xz
get https://github.com/libass/libass/releases/download/0.14.0/libass-0.14.0.tar.xz libass.tar.xz
get https://ffmpeg.org/releases/ffmpeg-6.1.tar.xz ffmpeg.tar.xz
get https://github.com/mpv-player/mpv/archive/refs/tags/v0.36.0.tar.gz mpv.tar.gz
get https://github.com/mesonbuild/meson/releases/download/1.5.2/meson-1.5.2.tar.gz meson.tar.gz   # pure Python, run in place
ls -la
