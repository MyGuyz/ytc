#!/bin/bash
# Cross-build YTC's dependencies for the Brick (tg5040, aarch64) inside the NX Redux
# toolchain container. Everything installs to /opt/ytc inside the toolchain sysroot
# (the host's ~/ytc-deps/prefix is mounted there), so the toolchain's pkg-config
# sysroot handling finds it like any other sysroot library.
#   docker run --rm -v ~/ytc-deps:/deps -v ~/ytc-deps/prefix:$SYSROOT/opt/ytc ... build-deps.sh
set -eo pipefail
source ~/.bashrc
set -u
: "${SYSROOT:?}"
P=/opt/ytc                       # prefix as the device-side .pc files see it
D=$SYSROOT                       # DESTDIR: where it actually lands in the container
HOST=aarch64-nextui-linux-gnu
J=$(nproc)
W=/deps/work; mkdir -p $W /deps/stamps
export PKG_CONFIG_PATH="$SYSROOT$P/lib/pkgconfig:$PKG_CONFIG_PATH"
export CC=$HOST-gcc CXX=$HOST-g++ AR=$HOST-ar RANLIB=$HOST-ranlib STRIP=$HOST-strip
export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"
done_() { [ -f /deps/stamps/$1 ]; }
mark() { touch /deps/stamps/$1; echo "=== $1 done"; }
src() { rm -rf "$W/$1"; mkdir -p "$W/$1"; tar xf "/deps/src/$2" -C "$W/$1" --strip-components=1; cd "$W/$1"; }

if ! done_ zlib; then src zlib zlib.tar.gz
  CHOST=$HOST ./configure --prefix=$P --static >/dev/null && make -j$J >/dev/null && make install DESTDIR=$D >/dev/null; mark zlib; fi

if ! done_ mbedtls; then src mbedtls mbedtls.tar.gz
  cmake -S . -B b -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=$CC -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$P -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF >/dev/null
  cmake --build b -j$J >/dev/null && DESTDIR=$D cmake --install b >/dev/null; mark mbedtls; fi

if ! done_ curl; then src curl curl.tar.gz
  ./configure --host=$HOST --prefix=$P --disable-shared --enable-static \
    --with-mbedtls=$D$P --with-zlib=$D$P --with-ca-bundle=/mnt/SDCARD/.system/shared/ssl/ca-certificates.crt \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-brotli --without-zstd \
    --without-libssh2 --without-librtmp --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
    --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
    --disable-mqtt --disable-manual >/dev/null
  make -j$J >/dev/null && make install DESTDIR=$D >/dev/null
  # the installed .pc points at the DESTDIR path for mbedtls: make it sysroot-relative
  sed -i "s|$D$P|$P|g" $D$P/lib/pkgconfig/libcurl.pc; mark curl; fi

if ! done_ fribidi; then src fribidi fribidi.tar.xz
  ./configure --host=$HOST --prefix=$P --disable-shared --enable-static --disable-docs >/dev/null 2>&1 \
    && make -j$J >/dev/null && make install DESTDIR=$D >/dev/null; mark fribidi; fi

if ! done_ libass; then src libass libass.tar.xz
  ./configure --host=$HOST --prefix=$P --disable-shared --enable-static --disable-harfbuzz \
    --disable-fontconfig --disable-require-system-font-provider --disable-asm >/dev/null
  make -j$J >/dev/null && make install DESTDIR=$D >/dev/null; mark libass; fi

if ! done_ ffmpeg; then src ffmpeg ffmpeg.tar.xz
  ./configure --prefix=$P --enable-cross-compile --cross-prefix=$HOST- --arch=aarch64 --target-os=linux \
    --sysroot=$SYSROOT --pkg-config=pkg-config --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-debug --enable-network --enable-openssl \
    --disable-encoders --enable-encoder=aac --disable-devices --disable-filters \
    --enable-filter=aresample,aformat,anull,null,format,scale,volume,atempo,hflip,vflip,transpose,rotate \
    --extra-cflags="-O2" >/dev/null
  make -j$J >/dev/null && make install DESTDIR=$D >/dev/null; mark ffmpeg; fi

if ! done_ mpv; then
  rm -rf $W/meson && mkdir -p $W/meson && tar xf /deps/src/meson.tar.gz -C $W/meson --strip-components=1
  meson() { python3 $W/meson/meson.py "$@"; }   # the image has no pip; meson runs from its source
  src mpv mpv.tar.gz
  cat > cross.txt <<EOF
[binaries]
c = '$HOST-gcc'
cpp = '$HOST-g++'
ar = '$HOST-ar'
strip = '$HOST-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
sys_root = '$SYSROOT'
EOF
  meson setup build --cross-file cross.txt --prefix=$P --buildtype=release \
    -Dlibmpv=true -Dcplayer=false -Dgpl=false -Dlua=disabled -Djavascript=disabled -Dlcms2=disabled \
    -Dlibplacebo=disabled -Dx11=disabled -Dwayland=disabled -Degl=disabled -Dgbm=disabled -Ddrm=disabled \
    -Dplain-gl=enabled -Dgl=enabled -Dalsa=enabled -Dpulse=disabled -Dsdl2=disabled -Dvulkan=disabled \
    -Duchardet=disabled -Drubberband=disabled -Dlibarchive=disabled -Dlibbluray=disabled -Ddvdnav=disabled \
    -Dcdda=disabled -Djpeg=disabled -Dzimg=disabled -Dmanpage-build=disabled > /deps/mpv-setup.log 2>&1 \
    || { tail -30 /deps/mpv-setup.log; exit 1; }
  ninja -C build >/deps/mpv-build.log 2>&1 || { tail -30 /deps/mpv-build.log; exit 1; }
  DESTDIR=$D ninja -C build install >/dev/null; mark mpv; fi

echo "=== all deps built"
ls $D$P/lib | grep -E "\.so|\.a$" | head -40
