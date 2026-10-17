#!/bin/sh
# Disk images for the system tests (tests/qemu/run_tests.py), like a laptop's
# SSD next to Windows:
#   test.mpg          12 s of colour bars and a 440 Hz tone (MPEG-1 + MP2)
#   nvme-install.img  GPT: a 20 MiB and a 66 MiB partition, both never formatted
#                     (`install` formats the second)
#   nvme-files.img    the same, the second partition already FAT32 "TERMINALOS"
#                     with test.mpg on it (Terminal OS mounts it as /), and
#                     photo.jpg, anim.gif, tone.mp3, tone.wav, notes.dat (text
#                     with an unknown ending), blob.bin (not text) and clip.mp4,
#                     clip.mkv, clip.webm (H.264, HEVC, VP9) for Files and Videos
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

if [ ! -s "$out/tone.mp3" ]; then
    ffmpeg -loglevel error -y -f lavfi -i testsrc=size=800x600 -frames:v 1 "$out/photo.jpg"
    ffmpeg -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=5 -t 1 "$out/anim.gif"
    ffmpeg -loglevel error -y -f lavfi -i sine=frequency=440:sample_rate=44100 -t 20 -c:a libmp3lame -b:a 128k -metadata title="Test Tone" -metadata artist="Terminal OS" "$out/tone.mp3"
    ffmpeg -loglevel error -y -f lavfi -i sine=frequency=660:sample_rate=22050 -t 5 -ac 1 -c:a pcm_s16le "$out/tone.wav"
    printf 'Plain text with an ending no app claims.\n' > "$out/notes.dat"
    head -c 4096 /dev/urandom > "$out/blob.bin"
fi

if [ ! -s "$out/clip.webm" ]; then                 # what phones and the web make
    src="-f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 8"
    ffmpeg -loglevel error -y $src -c:v libx264 -profile:v high -pix_fmt yuv420p -c:a aac "$out/clip.mp4"
    ffmpeg -loglevel error -y $src -c:v libx265 -pix_fmt yuv420p10le -x265-params log-level=error -c:a libopus "$out/clip.mkv"
    ffmpeg -loglevel error -y $src -c:v libvpx-vp9 -b:v 1M -deadline realtime -c:a libopus "$out/clip.webm"
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
mcopy -i "$out/nvme-files.img@@$((P2_START * 512))" "$out/test.mpg" "$out/photo.jpg" "$out/anim.gif" "$out/tone.mp3" "$out/tone.wav" "$out/notes.dat" "$out/blob.bin" "$out/clip.mp4" "$out/clip.mkv" "$out/clip.webm" ::/

echo "test disks in $out"
