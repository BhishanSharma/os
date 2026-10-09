#!/bin/sh
# Build the gzipped FAT32 RAM disk image that GRUB loads as a boot module.
# Needs mtools (mformat, mcopy) and gzip; no root / mount required.
# Usage: scripts/mkramdisk.sh <files-dir> <size-MiB> <output.img.gz>
# FAT32 needs at least 65525 clusters: with 512-byte clusters that is 33 MiB.
set -eu
dir="$1"; mb="$2"; out="$3"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

img="$tmp/ramdisk.img"
dd if=/dev/zero of="$img" bs=1M count="$mb" 2>/dev/null
mformat -i "$img" -F -c 1 -v RAMDISK ::
for f in "$dir"/*; do
    [ -f "$f" ] && mcopy -i "$img" "$f" "::$(basename "$f")"
done
mkdir -p "$(dirname "$out")"
gzip -9 -n -c "$img" > "$out"
