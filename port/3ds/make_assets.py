"""Make the HOME menu icon, banner image and banner sound for the 3DS build.

Drawn here from text, so no game art is used or committed:

  python make_assets.py <out dir>

writes icon.png (48x48), banner.png (256x128) and banner.wav (a short
silence; bannertool wants a sound).
"""

import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def centred(draw, box, text, f, fill):
    x0, y0, x1, y1 = draw.textbbox((0, 0), text, font=f)
    w, h = x1 - x0, y1 - y0
    bx0, by0, bx1, by1 = box
    draw.text((bx0 + (bx1 - bx0 - w) / 2 - x0, by0 + (by1 - by0 - h) / 2 - y0),
              text, font=f, fill=fill)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    icon = Image.new("RGBA", (48, 48), (20, 40, 90, 255))
    d = ImageDraw.Draw(icon)
    d.rectangle((0, 36, 47, 47), fill=(230, 160, 30, 255))
    centred(d, (0, 2, 48, 36), "P2i", font(18), (255, 255, 255, 255))
    centred(d, (0, 36, 48, 48), "3DS", font(9), (20, 20, 20, 255))
    icon.save(os.path.join(out, "icon.png"))

    banner = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    d = ImageDraw.Draw(banner)
    d.rounded_rectangle((8, 16, 247, 111), 14, fill=(20, 40, 90, 255))
    centred(d, (8, 24, 248, 76), "PSP2i Recomp", font(28), (255, 255, 255, 255))
    centred(d, (8, 76, 248, 104), "New 3DS", font(16), (230, 160, 30, 255))
    banner.save(os.path.join(out, "banner.png"))

    rate, seconds = 22050, 1
    data = b"\x00\x00" * rate * seconds
    with open(os.path.join(out, "banner.wav"), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


if __name__ == "__main__":
    main()
