#!/bin/sh
# Disk images for the system tests (tests/qemu/run_tests.py), like a laptop's
# SSD next to Windows:
#   test.mpg          12 s of colour bars and a 440 Hz tone (MPEG-1 + MP2)
#   nvme-install.img  GPT: a 20 MiB and a 66 MiB partition, both never formatted
#                     (`install` formats the second)
#   nvme-files.img    the same, the second partition already FAT32 "TERMINALOS"
#                     with test.mpg on it (Terminal OS mounts it as /)
# Needs sfdisk, mtools and ffmpeg (all in the build image).
# Usage: scripts/mktestdisks.sh <output folder>
set -eu
out="$1"
mkdir -p "$out"

if [ ! -s "$out/test.mpg" ]; then
    ffmpeg -loglevel error -y -f lavfi -i testsrc=size=640x360:rate=30 \
        -f lavfi -i sine=frequency=440:sample_rate=44100 -t 12 \
        -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg "$out/test.mpg"
fi

DATA=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7          # "Basic data", as Windows makes them
P2_START=43008                                     # sectors
make_disk() {
    rm -f "$1"
    truncate -s 200M "$1"
    sfdisk -q "$1" <<EOF
label: gpt
start=2048, size=40960, type=$DATA
start=$P2_START, size=135168, type=$DATA
EOF
}

make_disk "$out/nvme-install.img"

make_disk "$out/nvme-files.img"
mformat -i "$out/nvme-files.img@@$((P2_START * 512))" -F -v TERMINALOS ::
mcopy -i "$out/nvme-files.img@@$((P2_START * 512))" "$out/test.mpg" ::/test.mpg

echo "test disks in $out"
