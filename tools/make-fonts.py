#!/usr/bin/env python3
"""Builds the stick's smooth fonts (VLW, the format M5GFX loads) from fonts/Tektur.ttf.

Run after changing a size, a weight or the character set: python3 tools/make-fonts.py
Needs Pillow. The .vlw files it writes are committed, so building the firmware does not need Python.
"""
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TTF = ROOT / "fonts" / "Tektur.ttf"

# Latin, Ukrainian Cyrillic and the few symbols the interface uses. No space: M5GFX sizes it itself.
TEXT = ([chr(c) for c in range(0x21, 0x7F)] + [chr(c) for c in range(0x410, 0x450)]
        + list("ЄєІіЇїҐґ’«»–—…°№·"))
DIGITS = list("0123456789:-.")

FONTS = {  # name: (pixel size, weight, characters)
    "t11": (11, 500, TEXT),   # footers, small labels
    "t14": (14, 500, TEXT),   # body text
    "t18": (18, 700, TEXT),   # titles that do not fit larger
    "t24": (24, 700, TEXT),   # titles
    "t32": (32, 700, TEXT),   # big titles
    "t44": (44, 700, DIGITS),  # the clock and alert timers
}


def glyph(font: ImageFont.FreeTypeFont, ch: str):
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")  # relative to the baseline origin; y0 < 0 above it
    w, h = max(0, x1 - x0), max(0, y1 - y0)
    image = Image.new("L", (max(w, 1), max(h, 1)), 0)
    if w and h:
        ImageDraw.Draw(image).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    return w, h, round(font.getlength(ch)), -y0, x0, image.tobytes()[: w * h]


def vlw(size: int, weight: int, chars: list[str]) -> bytes:
    font = ImageFont.truetype(str(TTF), size)
    font.set_variation_by_axes([100, weight])  # width, weight
    ascent, descent = font.getmetrics()
    glyphs = [(ord(c), *glyph(font, c)) for c in sorted(set(chars), key=ord)]  # M5GFX searches them in code order
    out = [struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent)]
    out += [struct.pack(">7i", code, h, w, adv, dy, dx, 0) for code, w, h, adv, dy, dx, _ in glyphs]
    out += [bits for *_, bits in glyphs]
    return b"".join(out)


if __name__ == "__main__":
    for name, (size, weight, chars) in FONTS.items():
        data = vlw(size, weight, chars)
        (ROOT / "fonts" / f"{name}.vlw").write_bytes(data)
        print(f"{name}.vlw  {size}px  weight {weight}  {len(set(chars))} glyphs  {len(data)} bytes")
