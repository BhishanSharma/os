#!/usr/bin/env python3
"""Pre-render the desktop's fonts into UIFONT.BIN (anti-aliased glyphs).

    python scripts/gen-uifont.py   (needs Pillow; the output is committed)

Fonts: Inter (interface) and JetBrains Mono (Terminal), both SIL OFL 1.1,
in third_party/fonts. Each face is one style at one scale (1x = a 16 pixel
line). Little-endian layout:

    "UIF2", u32 faces, u32 n_extra
    faces x { u8 style, u8 scale, u16 px, u16 line_h, u16 baseline, u32 glyphs_off }
    n_extra x u32: the characters after ASCII (code points, sorted)
    per face, at glyphs_off: (96 + n_extra) x { u16 advance (1/64 px), s16 x0, s16 y0,
                                                u8 w, u8 h, u32 bitmap_off }
    bitmaps: w*h bytes of coverage (0-255), rows top to bottom

Characters 32-127, then EXTRA (Latin with accents, Greek, Cyrillic,
punctuation, arrows, box drawing, shapes) where either font has them: a
character the face's font lacks comes from the other one. x0/y0 place the
bitmap from the pen position and the top of the line box.
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

# The characters beyond ASCII (UTF-8 text): ranges of code points.
EXTRA_RANGES = [
    (0xA1, 0xFF),                                # Latin-1: accents, currency, symbols
    (0x100, 0x17F),                              # Latin Extended-A (Central European, Turkish...)
    (0x18F, 0x18F), (0x192, 0x192), (0x1A0, 0x1A1), (0x1AF, 0x1B0), (0x1CD, 0x1DC),
    (0x218, 0x21B), (0x237, 0x237), (0x259, 0x259),
    (0x2C6, 0x2C7), (0x2D8, 0x2DD),
    (0x384, 0x38A), (0x38C, 0x38C), (0x38E, 0x3A1), (0x3A3, 0x3CE),   # Greek
    (0x400, 0x45F), (0x490, 0x491), (0x4D8, 0x4D9), (0x4E8, 0x4E9),  # Cyrillic
    (0x1EA0, 0x1EF9),                            # Vietnamese
    (0x2010, 0x2027), (0x2030, 0x203A), (0x2044, 0x2044), (0x2070, 0x2079), (0x2080, 0x2089),
    (0x20A1, 0x20BF),                            # currency (the euro, the rupee...)
    (0x2113, 0x2113), (0x2116, 0x2116), (0x2122, 0x2122), (0x2126, 0x2126), (0x212E, 0x212E),
    (0x2153, 0x215E),                            # fractions
    (0x2190, 0x2199), (0x21A9, 0x21AA), (0x21B5, 0x21B5), (0x21E7, 0x21E7),   # arrows
    (0x2202, 0x2202), (0x2206, 0x2206), (0x220F, 0x2212), (0x2215, 0x2215), (0x2219, 0x221A),
    (0x221E, 0x221E), (0x222B, 0x222B), (0x2248, 0x2248), (0x2260, 0x2265),   # maths
    (0x2303, 0x2303), (0x2318, 0x2318), (0x2325, 0x2326), (0x232B, 0x232B), (0x23CE, 0x23CE),
    (0x2500, 0x257F),                            # box drawing (terminal programs)
    (0x2580, 0x259F),                            # blocks
    (0x25A0, 0x25A1), (0x25AA, 0x25AB), (0x25B2, 0x25B3), (0x25B6, 0x25B7), (0x25BC, 0x25BD),
    (0x25C0, 0x25C1), (0x25C6, 0x25C7), (0x25CB, 0x25CB), (0x25CF, 0x25CF), (0x25E6, 0x25E6),
    (0x2605, 0x2606), (0x2610, 0x2612), (0x2660, 0x2667), (0x2713, 0x2718),
    (0xFFFD, 0xFFFD),                            # the replacement character
]
FALLBACK = {"Inter.ttf": "JetBrainsMono.ttf", "JetBrainsMono.ttf": "Inter.ttf"}


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


def render(f, px, code):
    """(advance in 1/64 px, x0, y0 from the pen and baseline, w, h, coverage bytes)"""
    ch = chr(code)
    adv = round(f.getlength(ch) * 64) if code != 127 else 0
    size = px * 4
    img = Image.new("L", (size, size), 0)
    ox, oy = px, px * 3                         # the pen, on the baseline
    if code != 127:
        ImageDraw.Draw(img).text((ox, oy), ch, font=f, fill=255, anchor="ls")
    box = img.getbbox()
    if not box:
        return adv, 0, 0, 0, 0, b""
    g = img.crop(box)
    return adv, box[0] - ox, box[1] - oy, g.size[0], g.size[1], g.tobytes()


def has(f, px, code, missing):
    """Whether the font draws code itself, not its 'missing' box (.notdef)."""
    return render(f, px, code) != missing


def main():
    extra = [c for lo, hi in EXTRA_RANGES for c in range(lo, hi + 1)]
    # Which font has each character (tested at 2x: a clearer picture).
    fonts = {}
    for name in FALLBACK:
        f = load(os.path.join(FONTS, name), PX * 2, 400)
        missing = render(f, PX * 2, 0x10FFFD)
        fonts[name] = {c for c in extra if has(f, PX * 2, c, missing)}
    extra = [c for c in extra if any(c in fonts[n] for n in fonts)]

    faces, glyph_tables, bitmaps = [], [], bytearray()
    for style, (name, weight, scales) in enumerate(STYLES):
        for scale in scales:
            px, line_h = PX * scale, 16 * scale
            f = load(os.path.join(FONTS, name), px, weight)
            other = load(os.path.join(FONTS, FALLBACK[name]), px, weight)
            ascent, descent = f.getmetrics()
            baseline = (line_h - (ascent + descent)) // 2 + ascent
            table = []
            for code in list(range(32, 128)) + extra:
                src = f if code < 128 or code in fonts[name] else other
                adv, x0, y0, w, h, bits = render(src, px, code)
                if name == "JetBrainsMono.ttf" and src is other and adv > 0:
                    adv = round(f.getlength("M") * 64)       # (one cell wide, like the rest)
                if not w:
                    table.append((adv, 0, 0, 0, 0, 0))
                    continue
                table.append((adv, x0, y0 + baseline, w, h, len(bitmaps)))
                bitmaps += bits
            faces.append((style, scale, px, line_h, baseline))
            glyph_tables.append(table)

    n_glyphs = 96 + len(extra)
    header = 12 + len(faces) * 12 + len(extra) * 4
    glyphs_at = header
    face_bytes = b""
    for face in faces:
        face_bytes += struct.pack("<BBHHHI", *face, glyphs_at)
        glyphs_at += n_glyphs * 12
    bitmap_base = glyphs_at
    table_bytes = b""
    for table in glyph_tables:
        for adv, x0, y0, w, h, off in table:
            table_bytes += struct.pack("<HhhBBI", adv, x0, y0, w, h, bitmap_base + off)
    extra_bytes = b"".join(struct.pack("<I", c) for c in extra)
    data = (b"UIF2" + struct.pack("<II", len(faces), len(extra)) + face_bytes + extra_bytes +
            table_bytes + bytes(bitmaps))
    with open(OUT, "wb") as out:
        out.write(data)
    print("%s: %d faces, %d characters, %d bytes" % (OUT, len(faces), 96 + len(extra), len(data)))


if __name__ == "__main__":
    main()
