# The Terminal OS toolchain: binutils, GCC (C and C++) and mlibc for the
# target x86_64-pc-terminalos, installed in /opt/terminalos, then the libraries
# programs build on (OpenSSL, curl). Built on top of the OS build image:
#     docker build -t myos-toolchain toolchain
#
# The layers go from what changes least to what changes most: editing the
# mlibc port (toolchain/mlibc/terminalos) rebuilds mlibc and what comes after
# it, not binutils or the compiler.
FROM myos-buildenv

# JOBS: parallel compiles. GCC's biggest files need about 1 GB each, and
# Docker Desktop's VM may have only ~4 GB: more jobs crash the build.
ARG JOBS=3

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip ninja-build texinfo libgmp-dev libmpfr-dev libmpc-dev \
        bison flex pkg-config wget ca-certificates file perl && \
    pip3 install --break-system-packages meson==1.6.1

ENV TARGET=x86_64-pc-terminalos \
    PREFIX=/opt/terminalos \
    SYSROOT=/opt/terminalos/sysroot
ENV PATH=/opt/terminalos/bin:$PATH

ADD --checksum=sha256:c50c0e7f9cb188980e2cc97e4537626b1672441815587f1eab69d2a1bfbef5d2 \
    https://ftp.gnu.org/gnu/binutils/binutils-2.45.tar.xz /src/
ADD --checksum=sha256:438fd996826b0c82485a29da03a72d71d6e3541a83ec702df4271f6fe025d24e \
    https://ftp.gnu.org/gnu/gcc/gcc-15.2.0/gcc-15.2.0.tar.xz /src/
ADD --checksum=sha256:56d62366a6a15bc3fd8741bebcb311ca543553f7b734b1e780aeae33691bd621 \
    https://github.com/managarm/mlibc/archive/refs/tags/v7.0.0.tar.gz /src/mlibc-7.0.0.tar.gz

COPY patch-sources.sh /tc/

# 1. binutils. Its patched config.sub is kept: autotools ports (curl...) need
# one that knows "terminalos".
RUN cd /src && tar xJf binutils-2.45.tar.xz && sh /tc/patch-sources.sh binutils binutils-2.45 && \
    cp binutils-2.45/config.sub /tc/config.sub && \
    mkdir build-binutils && cd build-binutils && \
    ../binutils-2.45/configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng && \
    (make -j$JOBS && make install) > /src/binutils.log 2>&1 || (tail -60 /src/binutils.log; false) && \
    cd .. && rm -rf build-binutils binutils-2.45

# 2. mlibc's headers (the compiler is built against them): only the port's
# meson.build is needed, so editing the port's code does not rebuild GCC.
COPY cross-file.txt /tc/
COPY mlibc/setup-port.sh /tc/mlibc/
COPY mlibc/terminalos/meson.build /tc/mlibc/terminalos/
RUN cd /src && mkdir mlibc && tar xzf mlibc-7.0.0.tar.gz -C mlibc --strip-components=1 && \
    sh /tc/mlibc/setup-port.sh mlibc && cd mlibc && \
    meson setup --cross-file /tc/cross-file.txt --prefix=/usr -Dheaders_only=true build-headers && \
    DESTDIR=$SYSROOT ninja -C build-headers install

# 3. GCC: the compiler and libgcc
COPY terminalos.h /tc/
RUN cd /src && tar xJf gcc-15.2.0.tar.xz && sh /tc/patch-sources.sh gcc gcc-15.2.0 && \
    mkdir build-gcc && cd build-gcc && \
    ../gcc-15.2.0/configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT \
        --enable-languages=c,c++ --disable-nls --disable-multilib --disable-shared \
        --enable-threads=posix --enable-tls --disable-default-pie --enable-initfini-array \
        --disable-libssp --disable-libsanitizer --disable-libquadmath --disable-libgomp \
        --disable-libvtv --disable-libitm && \
    (make -j$JOBS all-gcc all-target-libgcc && make install-gcc install-target-libgcc) > /src/gcc.log 2>&1 || \
    (tail -60 /src/gcc.log; false)

# 4. mlibc itself (static). Its bundled compiler headers (freestnd-c-hdrs) are
# an older GCC's; this GCC's own replace them.
COPY mlibc/ /tc/mlibc/
RUN cd /src && sh /tc/mlibc/setup-port.sh mlibc && cd mlibc && rm -rf build && \
    meson setup --cross-file /tc/cross-file.txt --prefix=/usr -Ddefault_library=static build && \
    cp -r "$(x86_64-pc-terminalos-gcc -print-file-name=include)/." subprojects/freestnd-c-hdrs/x86_64/include/ && \
    (ninja -j$JOBS -C build && DESTDIR=$SYSROOT ninja -C build install) > /src/mlibc.log 2>&1 || \
    (grep -B2 -A12 "FAILED\|error:" /src/mlibc.log | head -80; false)

# 5. libstdc++ (needs the whole C library to configure against). Its build
# directory is kept only until then.
RUN cd /src/build-gcc && (make -j$JOBS all-target-libstdc++-v3 && make install-target-libstdc++-v3) > /src/libstdcxx.log 2>&1 || \
    (tail -60 /src/libstdcxx.log; false)

RUN x86_64-pc-terminalos-g++ --version | head -1

# ---- Libraries and tools -------------------------------------------------------------

ADD --checksum=sha256:603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a \
    https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz /src/
ADD --checksum=sha256:8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5 \
    https://curl.se/download/curl-8.23.0.tar.xz /src/
ADD --checksum=sha256:a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505 \
    https://curl.se/ca/cacert-2026-09-25.pem /opt/terminalos/share/cacert.pem

# 6. OpenSSL 3.5 (LTS): TLS for curl and, later, the browser. Configured as
# Linux on x86_64 (the C library is Linux-compatible); randomness from
# /dev/urandom, which the mlibc port provides.
RUN cd /src && tar xzf openssl-3.5.9.tar.gz && cd openssl-3.5.9 && \
    ./Configure linux-x86_64 --cross-compile-prefix=x86_64-pc-terminalos- --prefix=/usr \
        --openssldir=/etc/ssl --libdir=lib no-shared no-dso no-async no-engine no-afalgeng \
        no-tests no-docs no-ui-console --with-rand-seed=devrandom > /src/openssl.log 2>&1 && \
    (make -j$JOBS build_sw && make DESTDIR=$SYSROOT install_sw) >> /src/openssl.log 2>&1 || \
    (tail -60 /src/openssl.log; false)

# 7. curl (libcurl and the curl tool), with OpenSSL, HTTP/1.1 only for now.
RUN cd /src && tar xJf curl-8.23.0.tar.xz && cd curl-8.23.0 && cp /tc/config.sub config.sub && \
    ./configure --host=$TARGET --prefix=/usr --with-sysroot=$SYSROOT \
        --disable-shared --enable-static --with-openssl=$SYSROOT/usr \
        --with-ca-bundle=/etc/ssl/cert.pem --without-libpsl --without-zlib --without-brotli \
        --without-zstd --without-libidn2 --without-nghttp2 --without-nghttp3 --without-ngtcp2 \
        --disable-ipv6 --disable-threaded-resolver --disable-unix-sockets --disable-ldap \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
        --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-docs \
        > /src/curl.log 2>&1 && \
    (make -j$JOBS && make DESTDIR=$SYSROOT install) >> /src/curl.log 2>&1 || \
    (grep -B3 -A10 "error" /src/curl.log | tail -80; false)

RUN ls -l $SYSROOT/usr/bin/curl $SYSROOT/usr/lib/libssl.a $SYSROOT/usr/lib/libcurl.a
