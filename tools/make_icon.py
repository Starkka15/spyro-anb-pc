"""Draws res/spyro.ico: a purple egg with a gold band, after the title screen's
dragon egg. Original art, so the release carries none of the game's.

    G:\\Python310\\python.exe tools\\make_icon.py
"""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'res', 'spyro.ico')
S = 1024  # drawn large, scaled down per size


def egg_mask(size):
    m = Image.new('L', (size, size), 0)
    px = m.load()
    cx, cy = size / 2, size * 0.53
    a, b = size * 0.36, size * 0.45  # half width, half height
    for y in range(size):
        for x in range(size):
            dy = (y - cy) / b
            # narrower at the top: width shrinks as y goes up
            w = a * (1 + 0.22 * dy) if dy < 0 else a
            dx = (x - cx) / w
            d = dx * dx + dy * dy
            if d <= 1:
                px[x, y] = 255
    return m.filter(ImageFilter.GaussianBlur(1.5))


def draw():
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    mask = egg_mask(S)
    # body: purple, lit from the upper left
    body = Image.new('RGBA', (S, S))
    bp = body.load()
    for y in range(S):
        for x in range(S):
            lx, ly = (x - S * 0.38) / S, (y - S * 0.30) / S
            t = min(1.0, math.sqrt(lx * lx + ly * ly) * 1.6)
            r = int(196 * (1 - t) + 70 * t)
            g = int(120 * (1 - t) + 24 * t)
            b = int(236 * (1 - t) + 120 * t)
            bp[x, y] = (r, g, b, 255)
    img.paste(body, (0, 0), mask)
    # gold band around the middle, following the egg's curve
    band = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(band)
    d.ellipse((S * 0.05, S * 0.50, S * 0.95, S * 0.78), fill=255)
    d.ellipse((S * 0.05, S * 0.58, S * 0.95, S * 0.86), fill=0)
    band = Image.composite(band, Image.new('L', (S, S), 0), mask)
    gold = Image.new('RGBA', (S, S), (238, 178, 48, 255))
    img.paste(gold, (0, 0), band)
    # spots
    d = ImageDraw.Draw(img)
    for (x, y, r) in ((0.40, 0.36, 0.035), (0.58, 0.30, 0.025), (0.62, 0.45, 0.03), (0.44, 0.88, 0.03),
                      (0.62, 0.86, 0.022)):
        d.ellipse((S * (x - r), S * (y - r), S * (x + r), S * (y + r)), fill=(236, 200, 255, 200))
    # highlight
    hl = Image.new('L', (S, S), 0)
    ImageDraw.Draw(hl).ellipse((S * 0.30, S * 0.16, S * 0.44, S * 0.34), fill=110)
    hl = hl.filter(ImageFilter.GaussianBlur(S * 0.02))
    img.paste(Image.new('RGBA', (S, S), (255, 255, 255, 255)), (0, 0), Image.composite(hl, Image.new('L', (S, S), 0), mask))
    return img


def main():
    big = draw()
    sizes = [16, 24, 32, 48, 64, 128, 256]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    big.resize((256, 256), Image.LANCZOS).save(OUT, sizes=[(s, s) for s in sizes])
    big.resize((256, 256), Image.LANCZOS).save(os.path.splitext(OUT)[0] + '.png')
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
