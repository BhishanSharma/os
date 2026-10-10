#!/usr/bin/env python3
"""Draw the sample pictures that go on the RAM disk (targets/x86_64/ramdisk):
SAMPLE.PNG, a sunset over mountains, and TESTCARD.BMP, colour bars.
Python standard library only; run it again to regenerate them."""
import os, random, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "targets", "x86_64", "ramdisk")


def write_png(path, w, h, pixel):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                                   # filter: none (zlib does the work)
        for x in range(w):
            raw += bytes(pixel(x, y))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(data)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def sunset(w=640, h=400):
    random.seed(7)
    stars = {(random.randrange(w), random.randrange(h // 3)) for _ in range(120)}
    sun_x, sun_y, sun_r = w * 0.62, h * 0.58, h * 0.13
    # Three mountain ridges: sums of sines, nearer ones darker and lower.
    import math
    ridges = []
    for i, (base, amp, col) in enumerate([(0.55, 0.10, (70, 40, 90)), (0.66, 0.08, (45, 25, 60)),
                                           (0.78, 0.06, (22, 12, 30))]):
        phase = random.random() * 6
        ridges.append([h * base - h * amp * (math.sin(x / w * 7 + phase) * 0.6 +
                                             math.sin(x / w * 19 + phase * 2) * 0.3 +
                                             math.sin(x / w * 41 + phase * 3) * 0.1) for x in range(w)])
        ridges[-1] = (ridges[-1], col)

    def pixel(x, y):
        t = y / h
        sky = lerp((20, 24, 70), (250, 120, 60), t / 0.6) if t < 0.6 else lerp((250, 120, 60), (255, 200, 120), (t - 0.6) / 0.4)
        if (x, y) in stars and t < 0.3:
            sky = (255, 255, 230)
        d = math.hypot(x - sun_x, y - sun_y)
        if d < sun_r:
            sky = lerp((255, 240, 170), (255, 190, 90), d / sun_r)
        elif d < sun_r * 1.6:
            glow = 1 - (d - sun_r) / (sun_r * 0.6)
            sky = lerp(sky, (255, 210, 140), glow * 0.5)
        for line, col in ridges:
            if y >= line[x]:
                shade = min(1.0, (y - line[x]) / (h * 0.5))
                sky = lerp(col, (10, 5, 15), shade)
        return sky

    write_png(os.path.join(OUT, "sample.png"), w, h, pixel)


def testcard(w=240, h=160):
    bars = [(192, 192, 192), (192, 192, 0), (0, 192, 192), (0, 192, 0),
            (192, 0, 192), (192, 0, 0), (0, 0, 192)]
    stride = (w * 3 + 3) // 4 * 4
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if y < h * 2 // 3:
                r, g, b = bars[x * len(bars) // w]
            else:
                v = x * 255 // (w - 1)                     # grey ramp
                r = g = b = v
            row += bytes((b, g, r))
        rows.append(bytes(row) + bytes(stride - len(row)))
    body = b"".join(reversed(rows))                       # bottom-up
    header = b"BM" + struct.pack("<IHHI", 54 + len(body), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    open(os.path.join(OUT, "testcard.bmp"), "wb").write(header + dib + body)


if __name__ == "__main__":
    sunset()
    testcard()
    print("wrote sample.png and testcard.bmp to", os.path.normpath(OUT))
