#!/bin/sh
# build-ffmpeg.sh SRC OUT - build FFmpeg's decoders for Terminal OS programs
# (the video player, apps/video/ff.c) into OUT/lib and OUT/include.
#
# Only what playing needs: the MP4/MOV, Matroska/WebM, AVI, MP3, AAC, FLAC,
# Ogg and WAV readers and the H.264, HEVC, MPEG-4, VP8, VP9, AAC, MP3, MP2,
# Opus, Vorbis, FLAC and ALAC decoders, with FFmpeg's x86 assembly (SSE/AVX2,
# chosen at run time), decoding on several threads (the C library's pthreads).
# No files or network: the program does those.
# It is compiled against the OS's own C library (user/include, libc.a), so
# build that first (make user). LGPL 2.1: see SRC/COPYING.LGPLv2.1.
set -eu
SRC=$(cd "$1" && pwd)
OUT=$2
ROOT=$(pwd)
mkdir -p "$OUT/build"
OUT=$(cd "$OUT" && pwd)
cd "$OUT/build"
"$SRC/configure" --prefix="$OUT" \
  --enable-cross-compile --arch=x86_64 --target-os=none \
  --cc=x86_64-elf-gcc --ar=x86_64-elf-ar --ranlib=x86_64-elf-ranlib --nm=x86_64-elf-nm \
  --x86asmexe=nasm --pkg-config=false \
  --disable-autodetect --disable-everything --disable-programs --disable-doc \
  --disable-network --disable-w32threads --disable-os2threads \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
  --disable-debug --disable-hwaccels --enable-static --disable-shared \
  --enable-decoder=h264,hevc,mpeg4,vp8,vp9,aac,aac_latm,mp3float,mp2float,opus,vorbis,flac,alac,pcm_s16le,pcm_s24le,pcm_f32le \
  --enable-demuxer=mov,matroska,avi,mp3,aac,flac,ogg,wav \
  --enable-parser=h264,hevc,aac,mpeg4video,vp8,vp9,mpegaudio,opus,vorbis,flac \
  --extra-cflags="-I$ROOT/user/include -I$ROOT/src/intf -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables" \
  --extra-ldflags="-nostdlib -static -T $ROOT/user/user.ld $ROOT/build/user/crt0.o" \
  --extra-libs="$ROOT/build/user/libc.a -lgcc" > configure.log
# The OS has an mmap of its own (os.h: mmap(len, prot, fd)), not POSIX's.
sed -i "s/#define HAVE_MMAP 1/#define HAVE_MMAP 0/" config.h
make -k -j"$(nproc)" > make.log 2>&1 || { grep -B1 "error" make.log | head -40; exit 1; }
make install-headers install-libs > /dev/null
echo "FFmpeg in $OUT"
