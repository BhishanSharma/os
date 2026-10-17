# The environment Ladybird is built in: myos-skia plus Rust (the version
# Ladybird's rust-toolchain.toml names). Ladybird itself is built from a
# volume, step by step (toolchain/ladybird/build.sh), not in this image.
#     docker build -t myos-ladybird -f toolchain/ladybird/Dockerfile toolchain
FROM myos-skia

RUN apt-get update -qq && apt-get install -y -qq curl git ca-certificates > /dev/null && \
    curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.98.0 > /dev/null && \
    /root/.cargo/bin/rustc --version
ENV PATH=/root/.cargo/bin:$PATH

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
