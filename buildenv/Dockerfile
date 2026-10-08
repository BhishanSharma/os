FROM randomdude/gcc-cross-x86_64-elf

RUN apt-get update 
RUN apt-get upgrade -y
RUN apt-get install -y grub-common
RUN apt-get install -y nasm
RUN apt-get install -y grub-pc-bin
RUN apt-get install -y xorriso git

# Header-only BearSSL supplies a self-contained TLS 1.2 implementation.
RUN git clone --depth 1 --branch headeronly https://github.com/header-only/bearssl.git /root/bearssl

VOLUME /root/env
WORKDIR /root/env
