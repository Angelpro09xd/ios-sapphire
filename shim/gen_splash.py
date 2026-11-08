#!/usr/bin/env python3
"""The boot screen's picture for the shim (splash.c): PNG -> splash.bin.

  gen_splash.py splash/splash.png out/splash.bin [WIDTH]

Scaled to WIDTH (default 864) pixels across with Lanczos, rounded corners, composited on black, written
as "SPLH", u32 width, u32 height, then width * height a8r8g8b8 words (the panel's format), so the shim
only copies words.
"""
import struct, sys
from PIL import Image, ImageDraw

src, out = sys.argv[1], sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv) > 3 else 864
im = Image.open(src).convert('RGBA')
h = round(im.height * w / im.width)
im = im.resize((w, h), Image.LANCZOS)

# rounded corners, drawn at 4x and scaled down for a smooth edge
r, k = 40, 4
mask = Image.new('L', (w * k, h * k), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, w * k - 1, h * k - 1), r * k, fill=255)
mask = mask.resize((w, h), Image.LANCZOS)
alpha = Image.eval(Image.merge('L', [im.getchannel('A')]), lambda v: v)
alpha = Image.composite(alpha, Image.new('L', (w, h), 0), mask)

bg = Image.new('RGB', (w, h), (0, 0, 0))
bg.paste(im.convert('RGB'), (0, 0), alpha)

px = bg.tobytes()
words = bytearray(w * h * 4)
for i in range(w * h):
    r_, g_, b_ = px[3 * i], px[3 * i + 1], px[3 * i + 2]
    struct.pack_into('<I', words, 4 * i, 0xff000000 | r_ << 16 | g_ << 8 | b_)
with open(out, 'wb') as f:
    f.write(b'SPLH' + struct.pack('<II', w, h) + bytes(4) + words)
print('%s: %dx%d' % (out, w, h))
