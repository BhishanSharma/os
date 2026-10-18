#!/bin/sh
# Build Ladybird for Terminal OS, in the myos-ladybird image:
#   docker run --rm -v ladybird-src:/lb -v lb-build:/build \
#       -v "$PWD/toolchain/ladybird:/port" myos-ladybird sh /port/build.sh [target...]
# /lb/ladybird: Ladybird's source (a git checkout, see README.md); /build: the
# build folder, kept between runs. With no target it builds the program.
set -e
JOBS=${JOBS:-2}
# Package files for libraries that installed none (toolchain/ladybird/pkgconfig).
export PKG_CONFIG_PATH=/port/pkgconfig
cd /lb/ladybird
python3 /port/patch.py
cp -r /port/overlay/. .

# Programs link their static libraries as one group: Ladybird's libraries,
# its Rust archives and Skia name each other in both directions. Last in the
# group, what static Skia, curl and woff2 need (shared, they would bring it).
# Two Ladybird libraries each carry a copy of a Rust crate (as shared
# libraries they would keep them apart): the first copy is used.
LINK='<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--allow-multiple-definition -Wl,--start-group <LINK_LIBRARIES> -lfreetype -lnghttp2 -lwoff2common -lbrotlicommon -lpng -lz -Wl,--end-group'
if ! grep -q allow-multiple-definition /build/lb/CMakeCache.txt 2>/dev/null; then
    cmake -S . -B /build/lb -G Ninja \
        "-DCMAKE_CXX_LINK_EXECUTABLE=$LINK" \
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
        -DLIBAVIF_DIR=/port/cmake \
        -DLADYBIRD_CACHE_DIR=/build/caches
fi
# The C library is not among ninja's inputs: a newer one (the image's mlibc
# changed) means linking the program again.
[ /opt/terminalos/sysroot/usr/lib/libc.a -nt /build/lb/bin/Ladybird ] && rm -f /build/lb/bin/Ladybird
ninja -C /build/lb -j"$JOBS" -k 0 "${@:-ladybird}"
