#!/usr/bin/env python3
"""Pre-scale the desktop's icons into ICONS.BIN.

    python scripts/gen-icons.py   (needs Pillow; the output is committed)

Icons: Microsoft's Fluent UI Emoji (3D style, MIT licence) in
third_party/icons, and each app's own apps/NAME/icon.png (named NAME; square,
512 x 512 or more looks best). Each icon is stored at
48, 96 and 192 pixels square, 0xAARRGGBB (straight alpha), little-endian:

    "ICN1", u32 count
    count x { char name[16], 3 x { u32 size, u32 offset } }
    pixels
"""
import os
import struct
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "third_party", "icons")
FONTS = os.path.join(ROOT, "third_party", "fonts")
OUT = os.path.join(ROOT, "targets", "x86_64", "ramdisk", "ICONS.BIN")
SIZES = (48, 96, 192)
NAMES = ["folder", "mouse", "file", "info", "picture", "video", "gear", "computer", "user",
         "paint", "textedit", "activity", "sound", "wifi", "battery", "music", "terminal",
         "browser"]   # browser: the globe with meridians


def terminal_icon(size=512):
    """A dark rounded square with a green prompt, like a Mac's Terminal."""
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = size // 16
    d.rounded_rectangle((pad, pad + size // 40, size - pad, size - pad + size // 40), radius=size // 5,
                        fill=(0, 0, 0, 70))                                 # a soft shadow
    for y in range(pad, size - pad):                                          # dark grey, lighter at the top
        t = (y - pad) / (size - 2 * pad)
        c = int(70 - 40 * t)
        d.line((pad, y, size - pad, y), fill=(c, c, c + 6, 255))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((pad, pad, size - pad, size - pad), radius=size // 5, fill=255)
    body = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    body.paste(im, (0, 0), mask)
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((pad, pad + size // 40, size - pad, size - pad + size // 40),
                                             radius=size // 5, fill=(0, 0, 0, 60))
    out = Image.alpha_composite(shadow, body)
    font = ImageFont.truetype(os.path.join(FONTS, "JetBrainsMono.ttf"), size // 3)
    try:
        font.set_variation_by_axes([800])
    except Exception:
        pass
    ImageDraw.Draw(out).text((size * 0.2, size * 0.24), ">_", font=font, fill=(110, 230, 120, 255))
    return out


def main():
    images = {name: Image.open(os.path.join(ICONS, name + ".png")).convert("RGBA") for name in NAMES}
    # An app's own picture: apps/NAME/icon.png becomes the icon "NAME".
    apps = os.path.join(ROOT, "apps")
    for app in sorted(os.listdir(apps)) if os.path.isdir(apps) else []:
        png = os.path.join(apps, app, "icon.png")
        if os.path.isfile(png):
            if len(app) > 15:
                raise SystemExit("apps/%s: the folder name is too long for an icon name (15 at most)" % app)
            images[app] = Image.open(png).convert("RGBA")
    names = list(images)
    header = 8 + len(names) * (16 + len(SIZES) * 8)
    table, pixels = b"", bytearray()
    for name in names:
        table += name.encode().ljust(16, b"\0")
        for size in SIZES:
            im = images[name].resize((size, size), Image.LANCZOS)
            table += struct.pack("<II", size, header + len(pixels))
            pixels += im.tobytes("raw", "BGRA")                 # little-endian 0xAARRGGBB
    data = b"ICN1" + struct.pack("<I", len(names)) + table + bytes(pixels)
    with open(OUT, "wb") as f:
        f.write(data)
    print("%s: %d icons, %d bytes" % (OUT, len(names), len(data)))


if __name__ == "__main__":
    main()
