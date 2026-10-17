#!/bin/sh
# Lay out the built Ladybird as Terminal OS runs it, in /out (an empty
# folder), in the myos-ladybird image:
#   docker run --rm -v lb-build:/build -v "$PWD/toolchain/ladybird:/port" \
#       -v "$PWD/build/ladybird:/out" myos-ladybird sh /port/install.sh
# The result goes on the RAM disk as /ladybird (make LADYBIRD=build/ladybird):
#   bin/Ladybird        the program (the browser and its helper processes)
#   share/Lagom/        its resources (themes, icons, internal pages)
#   share/fonts/        the fonts pages get: Liberation (metric-compatible
#                       with Arial, Times New Roman, Courier New), Noto Emoji
set -e
mkdir -p /out/bin /out/share/fonts
x86_64-pc-terminalos-strip -o /out/bin/Ladybird /build/lb/bin/Ladybird
cp -r /build/lb/share/Lagom /out/share/
apt-get update -qq && apt-get install -y -qq fonts-liberation > /dev/null
cp /usr/share/fonts/truetype/liberation/*.ttf /out/share/fonts/
cp /out/share/Lagom/fonts/*.ttf /out/share/fonts/
du -sh /out/bin /out/share
