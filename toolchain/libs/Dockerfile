# Libraries for Terminal OS programs, on top of the toolchain image: zlib,
# libpng, libjpeg-turbo, FreeType, HarfBuzz, nghttp2 (curl gets HTTP/2 and
# gzip), ICU. All static, installed in the sysroot (/opt/terminalos/sysroot/usr).
#     docker build -t myos-libs -f toolchain/libs/Dockerfile toolchain
FROM myos-toolchain

ARG JOBS=3

RUN pip3 install --break-system-packages cmake==3.31.6

COPY cmake/ /opt/terminalos/share/cmake/
COPY libs/meson-cross.txt /opt/terminalos/share/meson-cross.txt

ENV TC=/opt/terminalos/share/cmake/TerminalOS.cmake \
    PKG_CONFIG_LIBDIR=/opt/terminalos/sysroot/usr/lib/pkgconfig \
    PKG_CONFIG_SYSROOT_DIR=/opt/terminalos/sysroot

# Each library: build quietly into /src/NAME.log; on failure show its end.
# cmake_lib DIR [options]: configure, build and install a CMake project.
RUN printf '%s\n' \
    '#!/bin/sh' \
    'set -e' \
    'name=$1; shift' \
    'cd /src/$name' \
    'cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \' \
    '    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib "$@" > /src/$name.log 2>&1 &&' \
    'ninja -j$JOBS -C build >> /src/$name.log 2>&1 &&' \
    'DESTDIR=$SYSROOT ninja -C build install >> /src/$name.log 2>&1 ||' \
    '{ grep -B2 -A12 -E "error|Error|FAILED" /src/$name.log | head -80; exit 1; }' \
    > /usr/local/bin/cmake_lib && chmod +x /usr/local/bin/cmake_lib

ADD --checksum=sha256:d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3 \
    https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz /src/
ADD --checksum=sha256:2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5 \
    https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz /src/libpng-1.6.59.tar.gz
ADD --checksum=sha256:6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e \
    https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz /src/
ADD --checksum=sha256:87c7f1f8699b91e89f773f7dcfa505b044e1dae67f62a55d4e29c3afdcac4fb8 \
    https://github.com/freetype/freetype/archive/refs/tags/VER-2-15-0.tar.gz /src/freetype-2.15.0.tar.gz
ADD --checksum=sha256:d07a007327277708a2a73ae437887cdbaf282937f6d03ca5467723e9099af586 \
    https://github.com/harfbuzz/harfbuzz/releases/download/14.6.0/harfbuzz-14.6.0.tar.xz /src/
ADD --checksum=sha256:e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2 \
    https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz /src/
ADD --checksum=sha256:3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0 \
    https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz /src/

# zlib
RUN cd /src && tar xJf zlib-1.3.2.tar.xz && mv zlib-1.3.2 zlib && \
    cmake_lib zlib -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF

# libpng
RUN cd /src && tar xzf libpng-1.6.59.tar.gz && mv libpng-1.6.59 libpng && \
    cmake_lib libpng -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_HARDWARE_OPTIMIZATIONS=ON

# libjpeg-turbo (with its SIMD code: NASM)
RUN cd /src && tar xzf libjpeg-turbo-3.2.0.tar.gz && mv libjpeg-turbo-3.2.0 libjpeg-turbo && \
    cmake_lib libjpeg-turbo -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON

# FreeType (PNG for colour emoji, zlib; HarfBuzz comes after it)
RUN cd /src && tar xzf freetype-2.15.0.tar.gz && mv freetype-VER-2-15-0 freetype && \
    cmake_lib freetype -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HARFBUZZ=ON

# HarfBuzz (text shaping) with FreeType
RUN cd /src && tar xJf harfbuzz-14.6.0.tar.xz && mv harfbuzz-14.6.0 harfbuzz && cd harfbuzz && \
    (meson setup build --cross-file /opt/terminalos/share/meson-cross.txt --prefix=/usr --libdir=lib \
        --buildtype=release -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
        -Dicu=disabled -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dintrospection=disabled && \
     ninja -j$JOBS -C build && DESTDIR=$SYSROOT ninja -C build install) > /src/harfbuzz.log 2>&1 || \
    (grep -B2 -A12 -E "error|ERROR|FAILED" /src/harfbuzz.log | head -80; false)

# nghttp2 (HTTP/2), then curl again: HTTP/2, gzip and deflate
RUN cd /src && tar xJf nghttp2-1.70.0.tar.xz && mv nghttp2-1.70.0 nghttp2 && \
    cmake_lib nghttp2 -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
RUN cd /src/curl-8.23.0 && make distclean > /dev/null 2>&1; \
    ./configure --host=$TARGET --prefix=/usr --with-sysroot=$SYSROOT \
        --disable-shared --enable-static --with-openssl=$SYSROOT/usr --with-zlib=$SYSROOT/usr \
        --with-nghttp2=$SYSROOT/usr --with-ca-bundle=/etc/ssl/cert.pem --without-libpsl --without-brotli \
        --without-zstd --without-libidn2 --without-nghttp3 --without-ngtcp2 \
        --disable-ipv6 --disable-threaded-resolver --disable-unix-sockets --disable-ldap \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
        --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-docs \
        > /src/curl2.log 2>&1 && \
    (make -j$JOBS && make DESTDIR=$SYSROOT install) >> /src/curl2.log 2>&1 || \
    (grep -B3 -A10 "error" /src/curl2.log | tail -80; false)

# ICU (Unicode: text segmentation, collation, number and date formats, IDNA).
# Its build makes and runs tools, so it is built twice: for this machine, then
# for Terminal OS using those tools. The data is one file, icudt78l.dat (ICU_DATA
# or u_setDataDirectory() says where), instead of 30 MB in every program.
# (ICU's makefiles have their own TARGET: the image's must not reach them.)
RUN cd /src && tar xzf icu4c-78.3-sources.tgz && \
    mkdir icu-host && cd icu-host && \
    (env -u TARGET ../icu/source/runConfigureICU Linux --disable-tests --disable-samples --disable-extras && \
     env -u TARGET make -j$JOBS) \
        > /src/icu-host.log 2>&1 || (tail -40 /src/icu-host.log; false)
RUN cd /src/icu/source && cp /tc/config.sub config.sub && cp config/mh-linux config/mh-unknown && \
    mkdir /src/icu-cross && cd /src/icu-cross && \
    (env -u TARGET ../icu/source/configure --host=x86_64-pc-terminalos --prefix=/usr --with-cross-build=/src/icu-host \
        --enable-static --disable-shared --disable-tests --disable-samples --disable-extras \
        --disable-tools --disable-dyload --with-data-packaging=archive && \
     env -u TARGET make -j$JOBS && env -u TARGET make DESTDIR=$SYSROOT install) > /src/icu.log 2>&1 || \
    (grep -B3 -A10 -E "error|Error" /src/icu.log | tail -80; false)

RUN ls $SYSROOT/usr/lib/*.a && ls -l $SYSROOT/usr/bin/curl

# The mlibc port again (in its build folder from myos-toolchain), so a change
# to it does not rebuild every library above: they are static archives, and
# the C library is only linked into programs.
COPY mlibc/ /tc/mlibc/
RUN cd /src && sh /tc/mlibc/setup-port.sh mlibc && cd mlibc && \
    (ninja -j$JOBS -C build && DESTDIR=$SYSROOT ninja -C build install) > /src/mlibc2.log 2>&1 || \
    (grep -B2 -A12 "FAILED\|error:" /src/mlibc2.log | head -80; false)
