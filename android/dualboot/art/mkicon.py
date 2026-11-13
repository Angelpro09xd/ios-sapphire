#!/usr/bin/env python3
"""mkicon.py LOGO PORTRAIT OUTDIR: the Sapphire Dual Boot icon.

The Android-Apple logo (white silhouette on black: an apple with antennae, eyes, a band and a bite) used as a
mask and filled with the portrait (Squidward as the Mona Lisa, the boot screen's picture); the logo's eyes and
band stay cut out. Behind it a dark "liquid glass" tile: a teal-to-violet gradient, a soft glow, a specular
highlight across the top and a thin light rim. Writes icon-1024.png and the launcher sizes into
OUTDIR/res/mipmap-*/ic_launcher.png (+ ic_launcher_round.png).
"""
import os, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter

N = 1024
logo_path, portrait_path, out = sys.argv[1:4]


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# the tile: diagonal gradient teal -> violet, darker at the bottom
bg = Image.new('RGB', (N, N))
px = bg.load()
c0, c1 = (18, 52, 70), (52, 22, 78)
for y in range(N):
    for x in range(N):
        t = (x * 0.45 + y * 0.55) / N
        r, g, b = lerp(c0, c1, t)
        k = 1.0 - 0.35 * (y / N)
        px[x, y] = (int(r * k), int(g * k), int(b * k))
bg = bg.convert('RGBA')

# the silhouette (white parts of the logo), scaled to 80 % of the tile
logo = Image.open(logo_path).convert('L')
box = logo.point(lambda v: 255 if v > 128 else 0).getbbox()
logo = logo.crop(box)
s = int(N * 0.80)
w, h = logo.size
scale = s / max(w, h)
logo = logo.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
mask = Image.new('L', (N, N), 0)
ox, oy = (N - logo.width) // 2, (N - logo.height) // 2 + int(N * 0.02)
mask.paste(logo, (ox, oy))
mask = mask.point(lambda v: 255 if v > 128 else 0).filter(ImageFilter.GaussianBlur(1.2))

# glow under the silhouette
glow = Image.new('RGBA', (N, N), (120, 230, 255, 0))
glow.putalpha(mask.filter(ImageFilter.GaussianBlur(40)).point(lambda v: int(v * 0.55)))
bg = Image.alpha_composite(bg, glow)

# the portrait: cover the silhouette's box, the face a little above its middle
p = Image.open(portrait_path).convert('RGBA')
sb = mask.point(lambda v: 255 if v > 128 else 0).getbbox()
bw, bh = sb[2] - sb[0], sb[3] - sb[1]
k = max(bw / p.width, bh / p.height) * 1.08
p = p.resize((int(p.width * k), int(p.height * k)), Image.LANCZOS)
fill = Image.new('RGBA', (N, N), (0, 0, 0, 0))
fill.paste(p, (sb[0] + (bw - p.width) // 2, sb[1] + (bh - p.height) // 2 - int(bh * 0.04)))
filled = Image.new('RGBA', (N, N), (0, 0, 0, 0))
filled.paste(fill, (0, 0), mask)
bg = Image.alpha_composite(bg, filled)

# a light rim around the silhouette
hard = mask.point(lambda v: 255 if v > 128 else 0)
rim = ImageChops.subtract(hard.filter(ImageFilter.MaxFilter(9)), hard)
rim_layer = Image.new('RGBA', (N, N), (235, 250, 255, 0))
rim_layer.putalpha(rim.filter(ImageFilter.GaussianBlur(1.5)).point(lambda v: int(v * 0.85)))
bg = Image.alpha_composite(bg, rim_layer)

# specular highlight: a soft white ellipse over the top of the tile
hi = Image.new('L', (N, N), 0)
ImageDraw.Draw(hi).ellipse((-N * 0.25, -N * 0.62, N * 1.25, N * 0.36), fill=255)
hi = hi.filter(ImageFilter.GaussianBlur(60))
grad = Image.new('L', (N, N))
grad.putdata([int(255 * max(0.0, 1.0 - y / (N * 0.42))) for y in range(N) for _ in range(N)])
hi = ImageChops.multiply(hi, grad).point(lambda v: int(v * 0.16))
bg = Image.alpha_composite(bg, Image.merge('RGBA', (Image.new('L', (N, N), 255),) * 3 + (hi,)))

# tile edge: a thin bright line, rounded (launchers mask the shape anyway)
edge = Image.new('L', (N, N), 0)
d = ImageDraw.Draw(edge)
d.rounded_rectangle((6, 6, N - 7, N - 7), radius=int(N * 0.22), outline=255, width=6)
edge = edge.filter(ImageFilter.GaussianBlur(2)).point(lambda v: int(v * 0.35))
bg = Image.alpha_composite(bg, Image.merge('RGBA', (Image.new('L', (N, N), 255),) * 3 + (edge,)))

os.makedirs(out, exist_ok=True)
bg.convert('RGB').save(os.path.join(out, 'icon-1024.png'))
for name, size in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    dd = os.path.join(out, 'res', 'mipmap-' + name)
    os.makedirs(dd, exist_ok=True)
    sq = bg.resize((size, size), Image.LANCZOS)
    sq.convert('RGB').save(os.path.join(dd, 'ic_launcher.png'))
    rm = Image.new('L', (size * 4, size * 4), 0)
    ImageDraw.Draw(rm).ellipse((0, 0, size * 4 - 1, size * 4 - 1), fill=255)
    rnd = sq.copy()
    rnd.putalpha(rm.resize((size, size), Image.LANCZOS))
    rnd.save(os.path.join(dd, 'ic_launcher_round.png'))
print('icon written to', out)
