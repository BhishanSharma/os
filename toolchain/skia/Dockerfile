# Skia (Ladybird's graphics library), the commit Ladybird's vcpkg uses, for
# Terminal OS: drawing on the CPU (Ganesh is built, with no GPU backends),
# fonts through FreeType, the system's libpng, libjpeg-turbo, libwebp, zlib.
# Static: /usr/lib/libskia.a, headers in /usr/include/skia, skia.pc.
#     docker build -t myos-skia -f toolchain/skia/Dockerfile toolchain
FROM myos-browser

ARG JOBS=3
ARG SKIA=e7c90ecca9444fe09598f1630ab7cee2c0ee027a

ADD --checksum=sha256:7b5158952ebc4e2ce8de19f036b7613e266705003469579d9e98070ead62fc3b \
    https://github.com/google/skia/archive/${SKIA}.tar.gz /src/skia.tar.gz

RUN cd /src && tar xzf skia.tar.gz && mv skia-${SKIA} skia && cd skia && python3 bin/fetch-gn

COPY skia/args.gn /src/skia/out/args.gn.in
RUN cd /src/skia && mkdir -p out/tos && sed "s|@SYSROOT@|$SYSROOT|g" out/args.gn.in > out/tos/args.gn && \
    (bin/gn gen out/tos && ninja -j$JOBS -C out/tos skia) > /src/skia.log 2>&1 || \
    (grep -B3 -A12 -E "error|ERROR|FAILED" /src/skia.log | head -100; false)

# Install: the library, the public headers (as include/... and core/...), a .pc
RUN cd /src/skia && cp out/tos/libskia.a $SYSROOT/usr/lib/ && \
    mkdir -p $SYSROOT/usr/include/skia && cp -r include $SYSROOT/usr/include/skia/ && \
    (cd include && find . -name '*.h' | cpio -pdm $SYSROOT/usr/include/skia 2>/dev/null) && \
    for d in modules src; do (find $d -name '*.h' | cpio -pdm $SYSROOT/usr/include/skia 2>/dev/null); done; \
    printf '%s\n' 'prefix=/usr' 'libdir=${prefix}/lib' 'includedir=${prefix}/include/skia' '' \
        'Name: skia' 'Description: 2D graphics library' 'Version: 148' \
        'Requires.private: freetype2 libpng libjpeg libwebp libwebpdemux libwebpmux zlib' \
        'Libs: -L${libdir} -lskia' \
        'Cflags: -I${includedir} -DSK_RELEASE -DSK_GANESH -DSK_DISABLE_LEGACY_SHADERCONTEXT' \
        > $SYSROOT/usr/lib/pkgconfig/skia.pc && ls -l $SYSROOT/usr/lib/libskia.a
