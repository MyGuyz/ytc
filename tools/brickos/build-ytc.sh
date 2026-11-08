#!/bin/bash
# Cross-build YTC (yt_ui) for the Brick inside the toolchain container, against the deps
# from build-deps.sh (mounted at $SYSROOT/opt/ytc), and assemble the app folder in /src/dist.
set -eo pipefail
source ~/.bashrc
set -u
HOST=aarch64-nextui-linux-gnu
P=$SYSROOT/opt/ytc
export PKG_CONFIG_PATH="$P/lib/pkgconfig:$PKG_CONFIG_PATH"
cd /src
cat > /tmp/tc.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $HOST-gcc)
set(CMAKE_CXX_COMPILER $HOST-g++)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
TLS="$P/lib/libcurl.a $P/lib/libmbedtls.a $P/lib/libmbedx509.a $P/lib/libmbedcrypto.a $P/lib/libz.a -lpthread"
cmake -S . -B build-brick -DCMAKE_TOOLCHAIN_FILE=/tmp/tc.cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_PLAYER=OFF \
  -DCMAKE_CXX_STANDARD_LIBRARIES="$TLS" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$P/lib -Wl,-rpath-link,$P/lib -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc" \
  > /tmp/cmake.log 2>&1 || { tail -30 /tmp/cmake.log; exit 1; }
grep -E "libmpv|libavformat|TLS" /tmp/cmake.log || true
cmake --build build-brick -j"$(nproc)" --target yt_ui > /tmp/make.log 2>&1 || { grep -E "error|Error" /tmp/make.log | head -30; exit 1; }

D=/src/dist/ytc
rm -rf /src/dist && mkdir -p $D/libs.aarch64
cp build-brick/yt_ui $D/ytc.aarch64 && $HOST-strip $D/ytc.aarch64
cp -a data $D/data
mkdir -p $D/config && cp config/clients.json $D/config/
# first-run defaults: Thai UI (index of "th" in data/lang/languages.json), 480p cap for software decode
TH=$(python3 -c "import json;print([l['code'] for l in json.load(open('data/lang/languages.json'))['languages']].index('th'))")
printf '{\n  "lang": %s,\n  "max_height": 480,\n  "autoplay": 0,\n  "home_source": 1,\n  "sponsorblock": 0,\n  "view": 0,\n  "volume": 100\n}\n' "$TH" > $D/config/settings.json
for l in libmpv.so.2 libavcodec.so.60 libavformat.so.60 libavutil.so.58 libswresample.so.4 libswscale.so.7 libavfilter.so.9; do
  cp -L $P/lib/$l $D/libs.aarch64/ && $HOST-strip $D/libs.aarch64/$l
done
cp LICENSE THIRD_PARTY_NOTICES.md $D/
du -sh $D; ls -la $D $D/libs.aarch64
