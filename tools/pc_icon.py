# A PC (monitor + tower) save icon, 64x64 RGBA, drawn 8x and scaled down.
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
K = 8
W = 64 * K
im = Image.new('RGBA', (W, W), (0, 0, 0, 0))

def P(pts): return [(x * K, y * K) for x, y in pts]

def poly(pts, c0, c1=None, axis=1):
    c1 = c1 or c0
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).polygon(P(pts), fill=255)
    g = Image.new('RGBA', (W, W))
    gd = ImageDraw.Draw(g)
    vs = [p[axis] for p in pts]
    a, b = int(min(vs) * K), int(max(vs) * K) + 1
    for i in range(a, b):
        t = (i - a) / max(1, b - a - 1)
        c = tuple(int(c0[j] + (c1[j] - c0[j]) * t) for j in range(4))
        gd.line([(0, i), (W, i)] if axis == 1 else [(i, 0), (i, W)], fill=c)
    im.paste(g, (0, 0), mask)

def line(pts, c, w=1.0):
    ImageDraw.Draw(im).line(P(pts), fill=c, width=max(1, int(w * K)))

def dot(x, y, r, c):
    ImageDraw.Draw(im).ellipse([(x - r) * K, (y - r) * K, (x + r) * K, (y + r) * K], fill=c)

# ---- tower, behind on the right ----
DX, DY = 7, -4
fx0, fx1, fy0, fy1 = 37, 52, 13, 55
poly([(fx0, fy0), (fx1, fy0), (fx1, fy1), (fx0, fy1)], (78, 80, 88, 255), (38, 39, 44, 255))
poly([(fx1, fy0), (fx1 + DX, fy0 + DY), (fx1 + DX, fy1 + DY), (fx1, fy1)], (46, 47, 53, 255), (20, 20, 24, 255))
poly([(fx0, fy0), (fx1, fy0), (fx1 + DX, fy0 + DY), (fx0 + DX, fy0 + DY)], (138, 140, 150, 255), (100, 102, 112, 255), axis=0)
# drive bays and vents
for y in (16.5, 19.5):
    poly([(39, y), (50, y), (50, y + 2), (39, y + 2)], (24, 24, 28, 255), (50, 51, 58, 255))
for y in range(25, 41, 2):
    line([(40.5, y), (48.5, y)], (28, 28, 32, 255), 0.6)
dot(44.5, 49.5, 1.6, (40, 40, 44, 255))
dot(44.5, 49.5, 1.1, (120, 230, 60, 255))           # power light

# ---- monitor, in front on the left ----
mx0, mx1, my0, my1 = 3, 40, 17, 45
MDX, MDY = 4, -2
# stand: neck and base
poly([(18, my1 - 1), (25, my1 - 1), (24, 51), (19, 51)], (60, 61, 68, 255), (34, 35, 40, 255))
poly([(10, 54), (33, 54), (36, 51), (13, 51)], (96, 98, 108, 255), (54, 55, 62, 255))
poly([(10, 54), (33, 54), (33, 56), (10, 56)], (30, 30, 34, 255))
poly([(33, 54), (36, 51), (36, 53), (33, 56)], (22, 22, 26, 255))
# body: side, top, front bezel
poly([(mx1, my0), (mx1 + MDX, my0 + MDY), (mx1 + MDX, my1 + MDY), (mx1, my1)], (44, 45, 51, 255), (20, 20, 24, 255))
poly([(mx0, my0), (mx1, my0), (mx1 + MDX, my0 + MDY), (mx0 + MDX, my0 + MDY)], (130, 132, 142, 255), (96, 98, 108, 255), axis=0)
poly([(mx0, my0), (mx1, my0), (mx1, my1), (mx0, my1)], (74, 76, 84, 255), (36, 37, 42, 255))
# screen: a dusky purple-to-blue glow, like the game's menus
sx0, sx1, sy0, sy1 = 6, 37, 20, 41
poly([(sx0, sy0), (sx1, sy0), (sx1, sy1), (sx0, sy1)], (150, 96, 220, 255), (40, 52, 140, 255))
# sheen across the glass
sheen = Image.new('RGBA', (W, W), (0, 0, 0, 0))
ImageDraw.Draw(sheen).polygon(P([(sx0, sy0), (sx0 + 13, sy0), (sx0 + 4, sy1), (sx0, sy1)]), fill=(255, 255, 255, 46))
clip = Image.new('L', (W, W), 0)
ImageDraw.Draw(clip).polygon(P([(sx0, sy0), (sx1, sy0), (sx1, sy1), (sx0, sy1)]), fill=255)
sheen.putalpha(ImageChops.multiply(sheen.getchannel('A'), clip))
im.alpha_composite(sheen)
line([(sx0, sy0), (sx1, sy0)], (20, 20, 24, 255), 0.7)             # inner bezel shadow
line([(sx0, sy0), (sx0, sy1)], (20, 20, 24, 255), 0.7)
dot(37.5, 43.2, 0.7, (120, 230, 60, 255))                           # monitor power light

# ---- dark outline so it reads on bright scenes ----
a = im.getchannel('A')
grown = a.filter(ImageFilter.MaxFilter(K * 2 + 1))
outline = Image.new('RGBA', (W, W), (8, 8, 10, 0))
outline.putalpha(grown.point(lambda v: v * 210 // 255))
outline.alpha_composite(im)
icon = outline.resize((64, 64), Image.LANCZOS)
icon.save(sys.argv[1])
bg = Image.new('RGBA', (64, 64), (40, 40, 40, 255)); bg.alpha_composite(icon)
bg.resize((256, 256), Image.NEAREST).save(sys.argv[2])
