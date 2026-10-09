FROM randomdude/gcc-cross-x86_64-elf

RUN apt-get update 
RUN apt-get upgrade -y
RUN apt-get install -y grub-common
RUN apt-get install -y nasm
RUN apt-get install -y grub-pc-bin
RUN apt-get install -y xorriso git
# UEFI boot: GRUB EFI modules for a hybrid BIOS+UEFI ISO (mtools builds its EFI image).
RUN apt-get install -y grub-efi-amd64-bin mtools

# Build the official BearSSL source library during the kernel build.
RUN git clone --depth 1 https://www.bearssl.org/git/BearSSL /root/bearssl

VOLUME /root/env
WORKDIR /root/env
