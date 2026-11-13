#!/usr/bin/env python3
"""Sapphire's boot screen, drawn from nothing (no outside artwork): a cut sapphire over a nebula.

  make_splash.py OUT_DIR [--preview]

Writes OUT_DIR/splash2.bin, what the shim carries and xnu (patch 0309) animates:

  "SPL2", u32 width, u32 height, u32 version (1)
  u32 gem_x, gem_y, gem_w, gem_h          the box the glow layer covers
  u32 word_x, word_y, word_w, word_h      the box of the wordmark's shimmer mask
  u32 ring_cx, ring_cy, ring_r, ring_n    the orbit ring; ring_n points follow
  u32 0 x 4
  base     width * height a8r8g8b8         the still frame
  glow     gem_w * gem_h a8r8g8b8          added (per channel, saturating) times the pulse, 0..255
  mask     word_w * word_h bytes           where the shimmer may light the wordmark
  ring     ring_n * (i16 dx, i16 dy)       the ring's points, in angle order

With --preview also OUT_DIR/splash2.png (the still frame) and OUT_DIR/splash2.gif (the animation, as
xnu's sweet_splash2_frame() computes it: the same integer arithmetic).
"""
import math, os, random, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1080, 2400
SS = 2                                   # supersampling for shapes
FONTS = "/mnt/c/Windows/Fonts/"
GEM_C = (540, 980)                       # the gem's centre
GEM_R = 250                              # its girdle radius
RING_R = 372

out = sys.argv[1]
preview = "--preview" in sys.argv
os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(1309)
random.seed(1309)


def font(name, size):
    for n in name:
        try:
            return ImageFont.truetype(FONTS + n, size)
        except OSError:
            pass
    return ImageFont.load_default()


def smooth_noise(h, w, scale, octaves=5):
    acc = np.zeros((h, w), np.float32)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        s = max(2, int(scale / (2 ** o)))
        small = rng.random((h // s + 2, w // s + 2)).astype(np.float32)
        img = Image.fromarray((small * 255).astype(np.uint8)).resize((w + s, h + s), Image.BICUBIC)
        acc += amp * (np.asarray(img, np.float32)[:h, :w] / 255.0)
        tot += amp
        amp *= 0.5
    return acc / tot


# ---------------------------------------------------------------- background: deep space, nebula, stars
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
bg = np.zeros((H, W, 3), np.float32)
t = yy / H
bg[..., 0] = 4 + 6 * t
bg[..., 1] = 6 + 4 * t
bg[..., 2] = 18 + 10 * (1 - abs(t - 0.4) * 2)

n1 = smooth_noise(H, W, 420)
n2 = smooth_noise(H, W, 260)
d = np.sqrt((xx - GEM_C[0]) ** 2 + ((yy - GEM_C[1]) * 0.8) ** 2)
fall = np.clip(1.25 - d / 900.0, 0, 1) ** 1.6
neb_b = np.clip((n1 - 0.42) * 2.6, 0, 1) * fall
neb_v = np.clip((n2 - 0.50) * 3.0, 0, 1) * fall
bg += neb_b[..., None] * np.array([10, 60, 170], np.float32)
bg += neb_v[..., None] * np.array([95, 30, 150], np.float32)
# a cyan haze low on the screen, under the wordmark
haze = np.exp(-((yy - 1700) / 420.0) ** 2) * np.exp(-((xx - 540) / 700.0) ** 2)
bg += haze[..., None] * np.array([0, 22, 40], np.float32)

stars = Image.new("RGB", (W, H), 0)
sd = ImageDraw.Draw(stars)
for _ in range(1400):
    x, y = random.random() * W, random.random() ** 1.1 * H
    b = random.random() ** 3
    r = 0.6 + b * 1.8
    c = int(90 + 165 * b)
    tint = random.choice([(c, c, c), (int(c * .8), int(c * .9), c), (c, int(c * .92), int(c * .8))])
    sd.ellipse((x - r, y - r, x + r, y + r), fill=tint)
stars_a = np.asarray(stars.filter(ImageFilter.GaussianBlur(0.6)), np.float32)
bg = bg + stars_a * 0.9


# ---------------------------------------------------------------- god rays behind the gem
rays = Image.new("L", (W, H), 0)
rd = ImageDraw.Draw(rays)
for i in range(28):
    a = i / 28 * 2 * math.pi + random.uniform(-0.05, 0.05)
    wdt = random.uniform(0.025, 0.06)
    L = random.uniform(700, 1150)
    p = [GEM_C, (GEM_C[0] + L * math.cos(a - wdt), GEM_C[1] + L * math.sin(a - wdt)),
         (GEM_C[0] + L * math.cos(a + wdt), GEM_C[1] + L * math.sin(a + wdt))]
    rd.polygon(p, fill=int(random.uniform(40, 110)))
rays = np.asarray(rays.filter(ImageFilter.GaussianBlur(14)), np.float32) / 255.0
rays *= np.clip(1.0 - d / 1050.0, 0, 1) ** 1.3
bg += rays[..., None] * np.array([40, 120, 255], np.float32) * 0.55


# ---------------------------------------------------------------- the gem: a brilliant cut, seen from above
def gem_layer():
    S = SS
    cx, cy, R = GEM_C[0] * S, GEM_C[1] * S, GEM_R * S
    img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    n = 8
    table = 0.50 * R
    star = 0.74 * R
    light = np.array([-0.45, -0.65, 0.62])
    light /= np.linalg.norm(light)

    def P(r, a):
        return (cx + r * math.cos(a), cy + r * math.sin(a))

    def shade(normal, base, spec=1.0):
        nrm = np.array(normal, float)
        nrm /= np.linalg.norm(nrm)
        k = max(0.0, float(nrm @ light))
        h = max(0.0, float(nrm @ (light + np.array([0, 0, 1])) / np.linalg.norm(light + np.array([0, 0, 1])))) ** 40
        c = np.array(base) * (0.28 + 0.95 * k) + spec * 255 * h * np.array([0.75, 0.92, 1.0])
        return tuple(int(min(255, v)) for v in c) + (255,)

    deep = (20, 70, 210)
    mid = (40, 130, 255)
    ice = (120, 210, 255)
    off = math.pi / n
    # girdle facets (outer ring of 16)
    for i in range(2 * n):
        a0 = i * math.pi / n - off / 2
        a1 = a0 + math.pi / n
        am = (a0 + a1) / 2
        p = [P(star, am), P(R, a0), P(R, a1)]
        nz = 0.55 if i % 2 else 0.42
        dr.polygon(p, fill=shade((math.cos(am) * 0.9, math.sin(am) * 0.9, nz), deep if i % 3 else mid))
    # kite (bezel) facets
    for i in range(n):
        a = i * 2 * math.pi / n
        p = [P(table, a - off), P(star, a - off * 0.0 - off), P(R * 0.995, a), P(star, a + off), P(table, a + off)]
        p = [P(table, a - off), P(R * 0.995, a), P(table, a + off), P(star * 0.80, a)]
        p = [P(table, a - off), P(star, a - off), P(R, a), P(star, a + off), P(table, a + off)]
        dr.polygon(p, fill=shade((math.cos(a) * 0.62, math.sin(a) * 0.62, 0.78), mid))
    # star facets
    for i in range(n):
        a = i * 2 * math.pi / n + off
        p = [P(table, a - off), P(star, a), P(table, a + off)]
        dr.polygon(p, fill=shade((math.cos(a) * 0.4, math.sin(a) * 0.4, 0.92), ice))
    # the table
    tp = [P(table, i * 2 * math.pi / n + off) for i in range(n)]
    tp = [P(table, i * 2 * math.pi / n - off) for i in range(n)]
    dr.polygon(tp, fill=shade((0.05, 0.08, 1.0), (60, 150, 255), 0.6))
    # inner reflections on the table: a smaller rotated octagon, lighter
    ip = [P(table * 0.58, i * 2 * math.pi / n) for i in range(n)]
    dr.polygon(ip, fill=(95, 190, 255, 120))
    # facet edges: thin bright lines
    edges = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ed = ImageDraw.Draw(edges)
    for i in range(n):
        a = i * 2 * math.pi / n
        for (r0, a0, r1, a1) in ((table, a - off, table, a + off), (table, a - off, star, a - off),
                                 (star, a - off, R, a), (R, a), (table, a + off, star, a + off))[:5] if False else ():
            pass
        segs = [((table, a - off), (table, a + off)), ((table, a - off), (star, a - off)), ((star, a - off), (R, a)),
                ((star, a + off), (R, a)), ((table, a + off), (star, a + off)), ((R, a), (R, a + 2 * off))]
        for (r0, a0), (r1, a1) in segs:
            ed.line([P(r0, a0), P(r1, a1)], fill=(190, 235, 255, 150), width=2 * S)
    img = Image.alpha_composite(img, edges)
    return img.resize((W, H), Image.LANCZOS)


gem = gem_layer()
gem_a = np.asarray(gem, np.float32)
alpha = gem_a[..., 3:4] / 255.0
# the bloom behind the gem (part of the still frame), then the gem over it
bloom = np.exp(-(d / 330.0) ** 2)
bg += bloom[..., None] * np.array([20, 90, 230], np.float32) * 0.9
bg = bg * (1 - alpha) + gem_a[..., :3] * alpha

# orbit ring: a thin, faint ellipse-free circle
ring_img = Image.new("L", (W * SS, H * SS), 0)
ImageDraw.Draw(ring_img).ellipse((
    (GEM_C[0] - RING_R) * SS, (GEM_C[1] - RING_R) * SS, (GEM_C[0] + RING_R) * SS, (GEM_C[1] + RING_R) * SS),
    outline=255, width=3 * SS)
ring_m = np.asarray(ring_img.resize((W, H), Image.LANCZOS), np.float32) / 255.0
bg += ring_m[..., None] * np.array([70, 130, 200], np.float32) * 0.55
# tick marks on the ring, like a dial
tk = Image.new("L", (W * SS, H * SS), 0)
td = ImageDraw.Draw(tk)
for i in range(60):
    a = i / 60 * 2 * math.pi
    r0 = RING_R + (14 if i % 5 == 0 else 7)
    r1 = RING_R + 22 if i % 5 == 0 else RING_R + 12
    td.line([((GEM_C[0] + r0 * math.cos(a)) * SS, (GEM_C[1] + r0 * math.sin(a)) * SS),
             ((GEM_C[0] + r1 * math.cos(a)) * SS, (GEM_C[1] + r1 * math.sin(a)) * SS)], fill=255, width=2 * SS)
tk = np.asarray(tk.resize((W, H), Image.LANCZOS), np.float32) / 255.0
bg += tk[..., None] * np.array([60, 110, 170], np.float32) * 0.6


# ---------------------------------------------------------------- the wordmark and the lines under it
def text_mask(txt, f, spacing=0):
    w = sum(f.getbbox(c)[2] - f.getbbox(c)[0] if c != " " else f.size // 3 for c in txt) + spacing * (len(txt) - 1)
    asc, desc = f.getmetrics()
    m = Image.new("L", (w + 40, asc + desc + 40), 0)
    dr = ImageDraw.Draw(m)
    x = 20
    for c in txt:
        if c == " ":
            x += f.size // 3 + spacing
            continue
        bb = f.getbbox(c)
        dr.text((x - bb[0], 20), c, font=f, fill=255)
        x += bb[2] - bb[0] + spacing
    bb = m.getbbox()
    return m.crop(bb)


def put(mask, cy, color, glow=None, x=None):
    global bg
    m = np.asarray(mask, np.float32) / 255.0
    h, w = m.shape
    x0 = (W - w) // 2 if x is None else x
    y0 = int(cy - h / 2)
    if glow is not None:
        gm = Image.new("L", (W, H), 0)
        gm.paste(mask, (x0, y0))
        gm = np.asarray(gm.filter(ImageFilter.GaussianBlur(glow[0])), np.float32) / 255.0
        bg += gm[..., None] * np.array(glow[1], np.float32)
    sl = bg[y0:y0 + h, x0:x0 + w]
    col = color(h, w) if callable(color) else np.array(color, np.float32)
    bg[y0:y0 + h, x0:x0 + w] = sl * (1 - m[..., None]) + col * m[..., None]
    return x0, y0, w, h


def metal(h, w):
    g = np.linspace(0, 1, h)[:, None]
    top = np.array([245, 250, 255], np.float32)
    midc = np.array([160, 205, 255], np.float32)
    bot = np.array([235, 245, 255], np.float32)
    c = np.where(g[..., None] < 0.55, top + (midc - top) * (g[..., None] / 0.55),
                 midc + (bot - midc) * ((g[..., None] - 0.55) / 0.45))
    return np.broadcast_to(c, (h, w, 3))


word = text_mask("SAPPHIRE", font(["seguibl.ttf", "segoeuib.ttf", "arialbd.ttf"], 150), spacing=16)
WORD_Y = 1560
wx, wy, ww, wh = put(word, WORD_Y, metal, glow=(26, (20, 80, 200)))
word_mask = np.asarray(word, np.uint8)

sub = text_mask("iOS 16   •   REDMI NOTE 13", font(["seguisb.ttf", "segoeuib.ttf"], 44), spacing=8)
put(sub, WORD_Y + 130, (150, 185, 230))

line = Image.new("L", (520, 4), 0)
ImageDraw.Draw(line).rectangle((0, 1, 519, 2), fill=255)
la = np.asarray(line, np.float32) / 255.0 * np.clip(1 - np.abs(np.linspace(-1, 1, 520)) ** 2, 0, 1)
put(Image.fromarray((la * 255).astype(np.uint8)), WORD_Y + 205, (90, 160, 255))

put(text_mask("#iOS4Sapphire", font(["seguibl.ttf", "segoeuib.ttf"], 64), spacing=3), 1930,
    (120, 200, 255), glow=(16, (10, 60, 160)))
put(text_mask("Thanks for using this port — please consider donating",
              font(["seguisb.ttf", "segoeuib.ttf"], 34)), 2020, (175, 190, 215))
heart = "♥"
put(text_mask("Made with " + heart + " by Angelpro09_Devv", font(["seguisb.ttf", "segoeuib.ttf"], 36)), 2078,
    (205, 215, 235))
put(text_mask("HOLD  VOL+  FOR RECOVERY", font(["seguisb.ttf", "segoeuib.ttf"], 28), spacing=4), 2290, (95, 110, 140))

base = np.clip(bg, 0, 255).astype(np.uint8)

# ---------------------------------------------------------------- what xnu animates
# glow layer: the gem's own light, added on top while it pulses
gx0, gy0 = GEM_C[0] - 410, GEM_C[1] - 410
gw = gh = 820
gyy, gxx = np.mgrid[0:gh, 0:gw].astype(np.float32)
gd = np.sqrt((gxx - 410) ** 2 + (gyy - 410) ** 2)
ga = alpha[gy0:gy0 + gh, gx0:gx0 + gw]
glow = np.exp(-(gd / 250.0) ** 2)[..., None] * np.array([30, 110, 255], np.float32) * 0.85 * (1 - 0.8 * ga)
glow += ga * np.array([18, 40, 60], np.float32)
glow = np.clip(glow, 0, 255).astype(np.uint8)

# ring points in angle order (clockwise from the top)
ring_n = 720
ring = [(int(round(RING_R * math.sin(2 * math.pi * i / ring_n))),
         int(round(-RING_R * math.cos(2 * math.pi * i / ring_n)))) for i in range(ring_n)]


def argb(rgb):
    a = rgb.astype(np.uint32)
    return (0xff000000 | (a[..., 0] << 16) | (a[..., 1] << 8) | a[..., 2]).astype("<u4")


hdr = struct.pack("<4s3I", b"SPL2", W, H, 1)
hdr += struct.pack("<4I", gx0, gy0, gw, gh)
hdr += struct.pack("<4I", wx, wy, ww, wh)
hdr += struct.pack("<4I", GEM_C[0], GEM_C[1], RING_R, ring_n)
# sparkles: a 4-point star sprite (added, RGB) at a few points of the gem, twinkling in turn
SPR = 121
sy_, sx_ = np.mgrid[0:SPR, 0:SPR].astype(np.float32) - SPR // 2
rr = np.sqrt(sx_ ** 2 + sy_ ** 2)
star = (np.exp(-np.abs(sx_) / 1.6) * np.exp(-np.abs(sy_) / 22.0) + np.exp(-np.abs(sy_) / 1.6) * np.exp(-np.abs(sx_) / 22.0))
star = star * np.clip(1 - rr / (SPR / 2), 0, 1) + np.exp(-(rr / 6.0) ** 2) * 1.0
spr = np.clip(star[..., None] * np.array([230, 245, 255], np.float32), 0, 255).astype(np.uint8)
glints = [(GEM_C[0] - 120, GEM_C[1] - 150), (GEM_C[0] + 170, GEM_C[1] - 60), (GEM_C[0] - 40, GEM_C[1] + 190),
          (GEM_C[0] + 60, GEM_C[1] - 205)]
hdr += struct.pack("<2I", SPR, len(glints)) + bytes(8)
with open(os.path.join(out, "splash2.bin"), "wb") as f:
    f.write(hdr)
    f.write(argb(base).tobytes())
    f.write(argb(glow).tobytes())
    f.write(word_mask.tobytes())
    f.write(b"".join(struct.pack("<hh", dx, dy) for dx, dy in ring))
    f.write(argb(spr).tobytes())
    f.write(b"".join(struct.pack("<hh", x, y) for x, y in glints))
print("splash2.bin", os.path.getsize(os.path.join(out, "splash2.bin")))

if preview:
    Image.fromarray(base).save(os.path.join(out, "splash2.png"))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import splash_anim
    splash_anim.preview(os.path.join(out, "splash2.bin"), os.path.join(out, "splash2.gif"))
