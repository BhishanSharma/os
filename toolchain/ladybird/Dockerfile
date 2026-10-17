# The environment Ladybird is built in: myos-skia plus Rust (the version
# Ladybird's rust-toolchain.toml names). Ladybird itself is built from a
# volume, step by step (toolchain/ladybird/build.sh), not in this image.
#     docker build -t myos-ladybird -f toolchain/ladybird/Dockerfile toolchain
FROM myos-skia

ARG JOBS=3

RUN apt-get update -qq && apt-get install -y -qq curl git patch ca-certificates > /dev/null && \
    curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.98.0 > /dev/null && \
    /root/.cargo/bin/rustc --version
ENV PATH=/root/.cargo/bin:$PATH

# libpng again, with the APNG patch (animated PNGs; Ladybird requires it). It
# replaces the plain one: the same library with more in it.
RUN cd /src && curl -fsSL -o apng.patch.gz \
        https://downloads.sourceforge.net/project/libpng-apng/libpng16/1.6.59/libpng-1.6.59-apng.patch.gz && \
    echo "a27ed2769b3c8d1703267b807663aa8e543b62355237e223d7731662034dd126951b88117c1038dc976750cf515d7e0e26e4a9278bf951f96ec4ed8f74105db7  apng.patch.gz" | sha512sum -c - && \
    gzip -d apng.patch.gz && rm -rf libpng && tar xzf libpng-1.6.59.tar.gz && mv libpng-1.6.59 libpng && \
    (cd libpng && patch -p1 < ../apng.patch > /dev/null) && \
    cmake_lib libpng -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_HARDWARE_OPTIMIZATIONS=ON && grep -q png_get_acTL $SYSROOT/usr/include/png.h

# WebGL: ANGLE's public headers only (the GL types and constants Ladybird's
# sources use). Its EGL and GLES functions come from a stub library made when
# Ladybird is linked (toolchain/ladybird/build.sh): every call fails, so the
# Compositor has no GL context and pages get no WebGL.
RUN set -e; base=https://raw.githubusercontent.com/google/angle/chromium/7258/include; \
    inc=$SYSROOT/usr/include/angle; \
    for f in EGL/egl.h EGL/eglext.h EGL/eglext_angle.h EGL/eglplatform.h GLES2/gl2.h GLES2/gl2ext.h \
             GLES2/gl2ext_angle.h GLES2/gl2platform.h GLES3/gl3.h GLES3/gl31.h GLES3/gl32.h \
             GLES3/gl3platform.h KHR/khrplatform.h; do \
        mkdir -p $inc/$(dirname $f) && curl -fsSL -o $inc/$f $base/$f; \
    done; \
    printf '%s\n' 'prefix=/usr' 'includedir=${prefix}/include/angle' 'libdir=${prefix}/lib' '' \
        'Name: angle' 'Description: ANGLE headers; GL calls fail (no WebGL)' 'Version: 7258' \
        'Libs: -L${libdir} -langle_stub' 'Cflags: -I${includedir} -DEGL_NO_X11 -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES' \
        > $SYSROOT/usr/lib/pkgconfig/angle.pc

# mimalloc's header where an #include <mimalloc.h> finds it (its package
# names /usr/include/mimalloc-2.2, a path outside the sysroot).
RUN cp $SYSROOT/usr/include/mimalloc-2.2/*.h $SYSROOT/usr/include/

# Skia's public headers as Ladybird includes them (<core/SkCanvas.h>) as well
# as Skia itself does (<include/core/SkCanvas.h>).
RUN cp -r $SYSROOT/usr/include/skia/include/* $SYSROOT/usr/include/skia/ && \
    test -f $SYSROOT/usr/include/skia/core/SkBlender.h

# Skia's headers under modules/ (skcms: colour management) that its public
# headers include.
RUN cd /src/skia && find modules -name '*.h' -exec cp --parents {} $SYSROOT/usr/include/skia/ \; && \
    test -f $SYSROOT/usr/include/skia/modules/skcms/skcms.h

# mimalloc's library where -lmimalloc finds it (beside its header above).
RUN cp $SYSROOT/usr/lib/mimalloc-2.2/libmimalloc.a $SYSROOT/usr/lib/

# The ANGLE stub library: every EGL and GLES function the headers declare,
# each returning 0 (EGL_NO_DISPLAY, EGL_FALSE, no context), so WebGL is off.
RUN cd /tmp && inc=$SYSROOT/usr/include/angle && \
    grep -hoE '(EGLAPIENTRY|GL_APIENTRY) +(egl|gl)[A-Za-z0-9_]+ *\(' -r $inc | \
        sed -E 's/.*ENTRY +([A-Za-z0-9_]+).*/\1/' | sort -u | \
        sed 's/.*/long &(void) { return 0; }/' > angle_stub.c && \
    x86_64-pc-terminalos-gcc -O2 -c angle_stub.c -o angle_stub.o && \
    x86_64-pc-terminalos-ar rcs $SYSROOT/usr/lib/libangle_stub.a angle_stub.o && \
    test $(wc -l < angle_stub.c) -gt 300 && rm angle_stub.*

# Skia's path operations (union, intersection: Ladybird clips with them), a
# part of Skia libskia.a leaves out, added to it.
RUN cd /src/skia && ninja -j$JOBS -C out/tos pathops > /src/pathops.log 2>&1 && \
    x86_64-pc-terminalos-ar rs $SYSROOT/usr/lib/libskia.a out/tos/obj/src/pathops/*.o && \
    x86_64-pc-terminalos-nm -C $SYSROOT/usr/lib/libskia.a | grep -q " T Op(SkPath"

# mlibc again, with the port as it is now (glibc_compat.cpp: the Linux C
# library names Rust's standard library calls). The images below this one
# keep the mlibc they were built with; only the port's own files changed.
# Also: dlsym() in a static program (Rust's standard library looks up optional
# functions that way) finds nothing, instead of stopping on the program's
# missing symbol hash table.
COPY mlibc/ /tc/mlibc/
RUN cd /src && sh /tc/mlibc/setup-port.sh mlibc > /dev/null && cd mlibc && \
    f=options/rtld/generic/linker.cpp && \
    (grep -q "HashStyle::none) return" $f || \
     sed -i 's|^\tif (object->hashStyle == HashStyle::systemV) {|\tif (object->hashStyle == HashStyle::none) return frg::optional<ObjectSymbol>{};\n&|' $f) && \
    grep -q "HashStyle::none) return" $f && \
    (ninja -j$JOBS -C build && DESTDIR=$SYSROOT ninja -C build install) > /src/mlibc4.log 2>&1 || \
    (grep -B2 -A12 "FAILED\|error:" /src/mlibc4.log | head -60; false)
