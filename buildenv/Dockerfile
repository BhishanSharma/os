FROM randomdude/gcc-cross-x86_64-elf

RUN apt-get update 
RUN apt-get upgrade -y
RUN apt-get install -y grub-common
RUN apt-get install -y nasm
RUN apt-get install -y grub-pc-bin
RUN apt-get install -y xorriso git
# UEFI boot: GRUB EFI modules for a hybrid BIOS+UEFI ISO (mtools builds its EFI image).
RUN apt-get install -y grub-efi-amd64-bin mtools
# Terminal OS's own UEFI boot loader (boot/): a PE/COFF program, built with MinGW.
RUN apt-get install -y gcc-mingw-w64-x86-64 dosfstools fdisk
# The system tests' disk images (make test-assets): a test video, made with ffmpeg.
RUN apt-get install -y --no-install-recommends ffmpeg

# Build the official BearSSL source library during the kernel build.
RUN git clone --depth 1 https://www.bearssl.org/git/BearSSL /root/bearssl

# FFmpeg 7.1 (LGPL): the video player's decoders, built by scripts/build-ffmpeg.sh.
ADD --checksum=sha256:40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6 \
    https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz /root/ffmpeg-7.1.tar.xz
RUN mkdir /root/ffmpeg && tar xJf /root/ffmpeg-7.1.tar.xz -C /root/ffmpeg --strip-components=1 && rm /root/ffmpeg-7.1.tar.xz

VOLUME /root/env
WORKDIR /root/env
