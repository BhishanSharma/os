#!/usr/bin/env python3
# zebin.py FILE.bin [SECTION OUT] ... - list the sections of an ocloc output
# (an ELF file, "zebin"), or write sections to files.
import struct, sys

def sections(data):
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    hdrs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    def name(off):
        s = data[strtab[4] + off:]
        return s[:s.index(b'\0')].decode()
    return {name(h[0]): data[h[4]:h[4] + h[5]] for h in hdrs}

if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    secs = sections(data)
    if len(sys.argv) == 2:
        for n, b in secs.items():
            print(f'{n:32} {len(b)} bytes')
    for n, out in zip(sys.argv[2::2], sys.argv[3::2]):
        open(out, 'wb').write(secs[n])
