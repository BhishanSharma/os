#!/bin/sh
# mkusbimg.sh - the USB stick image: GPT with one EFI system partition (FAT32)
#
#   \EFI\BOOT\BOOTX64.EFI          the loader (what firmware starts from a stick)
#   \EFI\TERMINALOS\TERMOS.EFI     the loader again (the installer copies it)
#   \EFI\TERMINALOS\KERNEL.BIN     the kernel
#   \EFI\TERMINALOS\RAMDISK.GZ     the system files
#
# The partition is labelled "TERMOS USB", not TERMINALOS: that label marks the
# partition an installed system keeps its files on.
#
# Usage: mkusbimg.sh <image> <loader.efi> <kernel.bin> <ramdisk.gz>
set -e
OUT=$1 LOADER=$2 KERNEL=$3 RAMDISK=$4
# 64 MiB, or more when the files need it (the RAM disk with Ladybird): their
# size, twice over for the loader's copy, plus 32 MiB.
FILES_MB=$(( ($(wc -c < "$LOADER") + $(wc -c < "$KERNEL") + $(wc -c < "$RAMDISK")) / 1048576 ))
SIZE_MB=$(( FILES_MB + 32 > 64 ? FILES_MB + 32 : 64 ))
START=2048

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" 2>/dev/null
printf 'label: gpt\nstart=%s, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="Terminal OS"\n' "$START" |
    sfdisk -q "$OUT"
SECTORS=$(sfdisk -d "$OUT" | sed -n "s/.*start= *$START, size= *\([0-9]*\).*/\1/p")
mkfs.fat -F 32 -s 1 -n "TERMOS USB" --offset "$START" "$OUT" $((SECTORS / 2)) >/dev/null

export MTOOLS_SKIP_CHECK=1
I="$OUT@@$((START * 512))"
mmd -i "$I" ::/EFI ::/EFI/BOOT ::/EFI/TERMINALOS
mcopy -i "$I" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$I" "$LOADER" ::/EFI/TERMINALOS/TERMOS.EFI
mcopy -i "$I" "$KERNEL" ::/EFI/TERMINALOS/KERNEL.BIN
mcopy -i "$I" "$RAMDISK" ::/EFI/TERMINALOS/RAMDISK.GZ
