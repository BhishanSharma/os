# What Ladybird needs on top of myos-libs: brotli, woff2, libwebp, dav1d,
# libavif, libxml2, SQLite, simdutf, simdjson, fast_float, fmt, mimalloc,
# libtommath, libpsl, wuffs, FFmpeg (Ladybird's codecs, "-ladybird" names).
# All static, in the sysroot.
#     docker build -t myos-browser -f toolchain/browser/Dockerfile toolchain
# mlibc is rebuilt in the first step: a change to the port rebuilds all of this.
FROM myos-libs

ARG JOBS=3

# The mlibc port first (with its Linux option: memfd_create, madvise,
# eventfd...), so these libraries are built against its headers.
COPY mlibc/ /tc/mlibc/
RUN mkdir -p /opt/linux-headers && cp -r /usr/include/linux /usr/include/asm-generic /opt/linux-headers/ && \
    cp -r /usr/include/x86_64-linux-gnu/asm /opt/linux-headers/asm && \
    cp -r /opt/linux-headers/* $SYSROOT/usr/include/ && \
    cd /src && sh /tc/mlibc/setup-port.sh mlibc && cd mlibc && \
    meson configure build -Dlinux_kernel_headers=/opt/linux-headers > /src/mlibc3.log 2>&1 && \
    (ninja -j$JOBS -C build && DESTDIR=$SYSROOT ninja -C build install) > /src/mlibc3.log 2>&1 || \
    (grep -B2 -A12 "FAILED\|error:" /src/mlibc3.log | head -80; false)

ADD --checksum=sha256:816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec \
    https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz /src/brotli-1.2.0.tar.gz
ADD --checksum=sha256:add272bb09e6384a4833ffca4896350fdb16e0ca22df68c0384773c67a175594 \
    https://github.com/google/woff2/archive/refs/tags/v1.0.2.tar.gz /src/woff2-1.0.2.tar.gz
ADD --checksum=sha256:e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 \
    https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz /src/
ADD --checksum=sha256:2abfb0c89212e6e4733a54e0ae509ec00a5b845a6360946f918806e14aedb011 \
    https://code.videolan.org/videolan/dav1d/-/archive/1.5.4/dav1d-1.5.4.tar.bz2 /src/
ADD --checksum=sha256:2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d \
    https://github.com/AOMediaCodec/libavif/archive/refs/tags/v1.4.2.tar.gz /src/libavif-1.4.2.tar.gz
ADD --checksum=sha256:98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821 \
    https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz /src/
ADD --checksum=sha256:0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c \
    https://www.sqlite.org/2026/sqlite-autoconf-3530400.tar.gz /src/
ADD --checksum=sha256:ec707f17e5083999efbdaf8a9a08d35e71e955b35dbf4b8307d14a7d31e9697f \
    https://github.com/simdutf/simdutf/archive/refs/tags/v9.1.1.tar.gz /src/simdutf-9.1.1.tar.gz
ADD --checksum=sha256:09de4289a26d3a0453569901a3837450b5b98a5c317d635eeee8e2a82b1ebe6b \
    https://github.com/simdjson/simdjson/archive/refs/tags/v5.0.2.tar.gz /src/simdjson-5.0.2.tar.gz
ADD --checksum=sha256:7ff47ad261068517561beb0a7c1d57131f71b8be19bdbac2857c6a517a2eb53c \
    https://github.com/fastfloat/fast_float/archive/refs/tags/v8.3.1.tar.gz /src/fast_float-8.3.1.tar.gz
ADD --checksum=sha256:8b852bb5aa6e7d8564f9e81394055395dd1d1936d38dfd3a17792a02bebd7af0 \
    https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz /src/fmt-12.2.0.tar.gz
ADD --checksum=sha256:8e0ed89907a681276bff2e49e9a048b47ba51254ab60daf6b3c220acac456a95 \
    https://github.com/microsoft/mimalloc/archive/refs/tags/v2.2.7.tar.gz /src/mimalloc-2.2.7.tar.gz
ADD --checksum=sha256:296272d93435991308eb73607600c034b558807a07e829e751142e65ccfa9d08 \
    https://github.com/libtom/libtommath/releases/download/v1.3.0/ltm-1.3.0.tar.xz /src/
ADD --checksum=sha256:1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208 \
    https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz /src/
ADD --checksum=sha256:f7b0c960c7487040c49bedda07b49f37fe447540236ef2936a9964e9aaeeef71 \
    https://github.com/google/wuffs/archive/refs/tags/v0.3.5.tar.gz /src/wuffs-0.3.5.tar.gz
ADD --checksum=sha256:8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e \
    https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz /src/

# meson_lib DIR [options]: configure, build and install a Meson project.
RUN printf '%s\n' \
    '#!/bin/sh' \
    'set -e' \
    'name=$1; shift' \
    'cd /src/$name' \
    '(meson setup build --cross-file /opt/terminalos/share/meson-cross.txt --prefix=/usr --libdir=lib \' \
    '    --buildtype=release --default-library=static "$@" && ninja -j$JOBS -C build &&' \
    ' DESTDIR=$SYSROOT ninja -C build install) > /src/$name.log 2>&1 ||' \
    '{ grep -B2 -A12 -E "error|ERROR|FAILED" /src/$name.log | head -80; exit 1; }' \
    > /usr/local/bin/meson_lib && chmod +x /usr/local/bin/meson_lib

# Header-only and small ones
RUN cd /src && tar xzf fast_float-8.3.1.tar.gz && mv fast_float-8.3.1 fast_float && \
    cmake_lib fast_float -DFASTFLOAT_TEST=OFF && \
    tar xzf wuffs-0.3.5.tar.gz && mkdir -p $SYSROOT/usr/include/wuffs && \
    cp wuffs-0.3.5/release/c/wuffs-v0.3.c $SYSROOT/usr/include/wuffs/ && \
    tar xzf fmt-12.2.0.tar.gz && mv fmt-12.2.0 fmt && cmake_lib fmt -DFMT_TEST=OFF -DFMT_DOC=OFF && \
    tar xJf ltm-1.3.0.tar.xz && mv libtommath-1.3.0 libtommath && \
    cmake_lib libtommath -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF

# brotli, then woff2 (web fonts) with it
RUN cd /src && tar xzf brotli-1.2.0.tar.gz && mv brotli-1.2.0 brotli && \
    cmake_lib brotli -DBUILD_SHARED_LIBS=OFF -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUILD_TOOLS=OFF && \
    tar xzf woff2-1.0.2.tar.gz && mv woff2-1.0.2 woff2 && \
    cmake_lib woff2 -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DNOISY_LOGGING=OFF \
        -DCMAKE_CXX_FLAGS="-include cstdint" -DCMAKE_CXX_STANDARD_LIBRARIES=$SYSROOT/usr/lib/libbrotlicommon.a

# simdutf, simdjson
RUN cd /src && tar xzf simdutf-9.1.1.tar.gz && mv simdutf-9.1.1 simdutf && \
    cmake_lib simdutf -DBUILD_SHARED_LIBS=OFF -DSIMDUTF_TESTS=OFF -DSIMDUTF_TOOLS=OFF -DSIMDUTF_BENCHMARKS=OFF && \
    tar xzf simdjson-5.0.2.tar.gz && mv simdjson-5.0.2 simdjson && \
    cmake_lib simdjson -DBUILD_SHARED_LIBS=OFF -DSIMDJSON_DEVELOPER_MODE=OFF

# mimalloc (Ladybird's allocator)
RUN cd /src && tar xzf mimalloc-2.2.7.tar.gz && mv mimalloc-2.2.7 mimalloc && \
    cmake_lib mimalloc -DMI_BUILD_SHARED=OFF -DMI_BUILD_OBJECT=OFF -DMI_BUILD_TESTS=OFF -DMI_OVERRIDE=OFF

# libwebp; dav1d (AV1) and libavif
RUN cd /src && tar xzf libwebp-1.6.0.tar.gz && mv libwebp-1.6.0 libwebp && \
    cmake_lib libwebp -DBUILD_SHARED_LIBS=OFF -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
        -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=ON
RUN cd /src && tar xjf dav1d-1.5.4.tar.bz2 && mv dav1d-1.5.4 dav1d && \
    meson_lib dav1d -Denable_tools=false -Denable_tests=false -Denable_examples=false
RUN cd /src && tar xzf libavif-1.4.2.tar.gz && mv libavif-1.4.2 libavif && \
    cmake_lib libavif -DBUILD_SHARED_LIBS=OFF -DAVIF_CODEC_DAV1D=SYSTEM -DAVIF_LIBYUV=OFF \
        -DAVIF_LIBSHARPYUV=OFF -DAVIF_LIBXML2=OFF -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_TESTS=OFF

# libxml2, SQLite (its one-file source), libpsl (public suffixes, built in)
RUN cd /src && tar xJf libxml2-2.15.4.tar.xz && mv libxml2-2.15.4 libxml2 && \
    cmake_lib libxml2 -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_ICONV=OFF \
        -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF \
        -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_ZLIB=ON
RUN cd /src && tar xzf sqlite-autoconf-3530400.tar.gz && cd sqlite-autoconf-3530400 && \
    x86_64-pc-terminalos-gcc -O2 -c sqlite3.c -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
        -DSQLITE_ENABLE_FTS5 -o sqlite3.o && \
    x86_64-pc-terminalos-ar rcs $SYSROOT/usr/lib/libsqlite3.a sqlite3.o && \
    cp sqlite3.h sqlite3ext.h $SYSROOT/usr/include/ && \
    printf '%s\n' 'prefix=/usr' 'libdir=${prefix}/lib' 'includedir=${prefix}/include' '' \
        'Name: SQLite' 'Description: SQL database engine' 'Version: 3.53.4' \
        'Libs: -L${libdir} -lsqlite3' 'Libs.private: -lm -pthread' 'Cflags: -I${includedir}' \
        > $SYSROOT/usr/lib/pkgconfig/sqlite3.pc
RUN cd /src && tar xzf libpsl-0.21.5.tar.gz && mv libpsl-0.21.5 libpsl && \
    meson_lib libpsl -Druntime=no -Dbuiltin=true -Dtests=false -Ddocs=false

# FFmpeg: Ladybird's decoders only, its library names (libavcodec-ladybird...)
RUN cd /src && tar xJf ffmpeg-9.0.2.tar.xz && cd ffmpeg-9.0.2 && \
    (./configure --prefix=/usr --enable-cross-compile --cross-prefix=x86_64-pc-terminalos- \
        --arch=x86_64 --target-os=linux --extra-cflags=-D_GNU_SOURCE --pkg-config=pkg-config --pkg-config-flags=--static \
        --build-suffix=-ladybird --enable-static --disable-shared --disable-programs --disable-doc \
        --disable-everything --disable-network --disable-autodetect --enable-pthreads --enable-zlib \
        --enable-libdav1d --enable-swresample --disable-swscale --disable-avdevice --disable-avfilter \
        --enable-decoder=vp8,vp9,libdav1d,theora,vorbis,opus,flac,mp3float,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_alaw,pcm_mulaw \
        --enable-parser=vp8,vp9,av1,flac,mpegaudio --enable-demuxer=ogg,flac,wav,matroska,mov,mp3 && \
     make -j$JOBS && make DESTDIR=$SYSROOT install) > /src/ffmpeg.log 2>&1 || \
    (grep -B3 -A10 -E "error|ERROR" /src/ffmpeg.log | tail -60; tail -30 /src/ffmpeg-9.0.2/ffbuild/config.log; false)

RUN ls $SYSROOT/usr/lib/*.a | wc -l && ls $SYSROOT/usr/lib/pkgconfig/
