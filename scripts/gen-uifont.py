#!/usr/bin/env python3
"""Pre-render the desktop's fonts into UIFONT.BIN (anti-aliased glyphs).

    python scripts/gen-uifont.py   (needs Pillow; the output is committed)

Fonts: Inter (interface) and JetBrains Mono (Terminal), both SIL OFL 1.1,
in third_party/fonts. Each face is one style at one scale (1x = a 16 pixel
line). Little-endian layout:

    "UIF1", u32 faces
    faces x { u8 style, u8 scale, u16 px, u16 line_h, u16 baseline, u32 glyphs_off }
    per face, at glyphs_off: 96 x { u16 advance (1/64 px), s16 x0, s16 y0, u8 w, u8 h, u32 bitmap_off }
    bitmaps: w*h bytes of coverage (0-255), rows top to bottom

Characters 32-127; x0/y0 place the bitmap from the pen position and the top
of the line box.
"""
import os
import struct
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "third_party", "fonts")
OUT = os.path.join(ROOT, "targets", "x86_64", "ramdisk", "UIFONT.BIN")

# style: (file, weight, scales)
STYLES = [
    ("Inter.ttf", 430, (1, 2, 3, 4)),          # 0 regular
    ("Inter.ttf", 640, (1, 2, 3, 4)),          # 1 bold
    ("JetBrainsMono.ttf", 400, (1, 2)),        # 2 mono
]
PX = 13                                         # at 1x, in a 16 pixel line


def load(path, px, weight):
    f = ImageFont.truetype(path, px)
    axes = f.get_variation_axes()
    values = []
    for a in axes:
        name = a["name"].decode() if isinstance(a["name"], bytes) else a["name"]
        if name.lower().startswith("weight"):
            values.append(weight)
        elif name.lower().startswith("optical"):
            values.append(max(a["minimum"], min(a["maximum"], px)))
        else:
            values.append(a["default"])
    if values:
        f.set_variation_by_axes(values)
    return f


def main():
    faces, glyph_tables, bitmaps = [], [], bytearray()
    for style, (name, weight, scales) in enumerate(STYLES):
        for scale in scales:
            px, line_h = PX * scale, 16 * scale
            f = load(os.path.join(FONTS, name), px, weight)
            ascent, descent = f.getmetrics()
            baseline = (line_h - (ascent + descent)) // 2 + ascent
            table = []
            for code in range(32, 128):
                ch = chr(code)
                adv = round(f.getlength(ch) * 64) if code < 127 else 0
                size = px * 4
                img = Image.new("L", (size, size), 0)
                ox, oy = px, px * 3                 # the pen, on the baseline
                if code < 127:
                    ImageDraw.Draw(img).text((ox, oy), ch, font=f, fill=255, anchor="ls")
                box = img.getbbox()
                if not box:
                    table.append((adv, 0, 0, 0, 0, 0))
                    continue
                g = img.crop(box)
                w, h = g.size
                table.append((adv, box[0] - ox, box[1] - oy + baseline, w, h, len(bitmaps)))
                bitmaps += g.tobytes()
            faces.append((style, scale, px, line_h, baseline))
            glyph_tables.append(table)

    header = 8 + len(faces) * 12
    glyphs_at = header
    face_bytes = b""
    for face in faces:
        face_bytes += struct.pack("<BBHHHI", *face, glyphs_at)
        glyphs_at += 96 * 12
    bitmap_base = glyphs_at
    table_bytes = b""
    for table in glyph_tables:
        for adv, x0, y0, w, h, off in table:
            table_bytes += struct.pack("<HhhBBI", adv, x0, y0, w, h, bitmap_base + off)
    data = b"UIF1" + struct.pack("<I", len(faces)) + face_bytes + table_bytes + bytes(bitmaps)
    with open(OUT, "wb") as out:
        out.write(data)
    print("%s: %d faces, %d bytes" % (OUT, len(faces), len(data)))


if __name__ == "__main__":
    main()
