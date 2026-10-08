#!/bin/sh
# Create a 32 MB FAT32 disk image with two sample files.
# Needs dosfstools (mkfs.fat) and mtools (mcopy). No root / mount required.
# Usage: scripts/mkdisk.sh [output-file]
set -eu
out="${1:-disk.img}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

rm -f "$out"
dd if=/dev/zero of="$out" bs=1M count=32 2>/dev/null
mkfs.fat -F 32 "$out" >/dev/null
echo "Hello from FAT32!"   > "$tmp/test.txt"
echo "Readme file content" > "$tmp/readme.txt"
mcopy -i "$out" "$tmp/test.txt"   ::test.txt
mcopy -i "$out" "$tmp/readme.txt" ::readme.txt
echo "Created $out:"
mdir -i "$out" ::
