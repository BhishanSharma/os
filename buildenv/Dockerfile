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

# Build the official BearSSL source library during the kernel build.
RUN git clone --depth 1 https://www.bearssl.org/git/BearSSL /root/bearssl

VOLUME /root/env
WORKDIR /root/env
