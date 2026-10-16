#!/usr/bin/env python3
"""Copy one folder's files out of a FAT32 partition image (read only).

    python scripts/fatlogs.py <partition.img> <FOLDER> <out-dir>

usb-logs.ps1 uses it for LOGS on the Terminal OS stick, so reading the logs
needs no Docker or mtools. Short (8.3) names only, which is what logd writes.
"""
import os
import struct
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    image, folder, out = sys.argv[1], sys.argv[2].upper(), sys.argv[3]
    with open(image, "rb") as f:
        disk = f.read()
    bps, spc, reserved, nfats = struct.unpack_from("<HBHB", disk, 11)
    fat_size = struct.unpack_from("<I", disk, 36)[0]
    root = struct.unpack_from("<I", disk, 44)[0]
    if disk[82:90] != b"FAT32   " or bps != 512:
        sys.exit("not a FAT32 partition")
    fat_at = reserved * bps
    data_at = (reserved + nfats * fat_size) * bps
    cluster_bytes = spc * bps

    def chain(c):
        while 2 <= c < 0x0FFFFFF8:
            yield c
            c = struct.unpack_from("<I", disk, fat_at + 4 * c)[0] & 0x0FFFFFFF

    def read(c, size=None):
        data = b"".join(disk[data_at + (k - 2) * cluster_bytes:][:cluster_bytes] for k in chain(c))
        return data if size is None else data[:size]

    def entries(c):
        d = read(c)
        for i in range(0, len(d), 32):
            e = d[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            name = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            first = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            yield (name + ("." + ext if ext else "")), e[11] & 0x10, first, struct.unpack_from("<I", e, 28)[0]

    found = [e for e in entries(root) if e[0] == folder and e[1]]
    if not found:
        print("no %s folder on the stick yet; its top folder has: %s"
              % (folder, ", ".join(e[0] + ("/" if e[1] else "") for e in entries(root))))
        sys.exit(2)
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, is_dir, first, size in entries(found[0][2]):
        if is_dir:
            continue
        with open(os.path.join(out, name), "wb") as f:
            f.write(read(first, size) if size else b"")
        n += 1
    print("%d file(s)" % n)


if __name__ == "__main__":
    main()
