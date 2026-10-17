#!/bin/sh
# Build Ladybird for Terminal OS, in the myos-ladybird image:
#   docker run --rm -v ladybird-src:/lb -v lb-build:/build \
#       -v "$PWD/toolchain/ladybird:/port" myos-ladybird sh /port/build.sh [target...]
# /lb/ladybird: Ladybird's source (a git checkout, see README.md); /build: the
# build folder, kept between runs. With no target it builds the program.
set -e
JOBS=${JOBS:-2}
cd /lb/ladybird
python3 /port/patch.py
cp -r /port/overlay/. .

if [ ! -f /build/lb/build.ninja ]; then
    cmake -S . -B /build/lb -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/terminalos/share/cmake/TerminalOS.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DTERMINALOS=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DENABLE_CRANELIFT_JIT=OFF \
        -DENABLE_JS_JIT=OFF \
        -DENABLE_LTO_FOR_RELEASE=OFF \
        -DENABLE_LAGOM_CCACHE=OFF \
        -DENABLE_INSTALL_HEADERS=OFF \
        -DENABLE_INSTALL_FREEDESKTOP_FILES=OFF \
        -DENABLE_COMPILETIME_FORMAT_CHECK=OFF \
        -DLADYBIRD_ENABLE_CPPTRACE=OFF \
        -DLADYBIRD_CACHE_DIR=/build/caches
fi
ninja -C /build/lb -j"$JOBS" -k 0 "${@:-ladybird}"
