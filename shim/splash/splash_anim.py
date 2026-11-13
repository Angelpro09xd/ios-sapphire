#!/usr/bin/env python3
"""The boot screen's animation, as xnu draws it (patch 0309, sweet_splash2_frame): same integer arithmetic,
so the preview is what the phone shows.

  splash_anim.py splash2.bin out.gif

Per frame at T milliseconds since the boot screen came up:
  - the gem pulses: in the gem box, base + glow * P / 256 per channel (saturating), P = 150 + SIN(T / 2400)
  - a comet runs round the ring every 2.8 s: TAIL points behind the head, brightness falling linearly
  - every 3 s a light sweeps the wordmark diagonally (1.4 s), only where its mask is set
"""
import math, struct, sys
import numpy as np
from PIL import Image

SIN = [int(round(127 * math.sin(2 * math.pi * i / 256))) for i in range(256)]
TAIL = 110
PULSE_MS, COMET_MS, SWEEP_MS, SWEEP_RUN, GLINT_MS = 2400, 2800, 3000, 1400, 2000


def load(path):
    b = open(path, "rb").read()
    magic, w, h, ver = struct.unpack_from("<4s3I", b, 0)
    assert magic == b"SPL2"
    gx, gy, gw, gh = struct.unpack_from("<4I", b, 16)
    wx, wy, ww, wh = struct.unpack_from("<4I", b, 32)
    cx, cy, r, n = struct.unpack_from("<4I", b, 48)
    o = 80
    base = np.frombuffer(b, "<u4", w * h, o).reshape(h, w); o += w * h * 4
    glow = np.frombuffer(b, "<u4", gw * gh, o).reshape(gh, gw); o += gw * gh * 4
    mask = np.frombuffer(b, np.uint8, ww * wh, o).reshape(wh, ww); o += ww * wh
    ring = np.frombuffer(b, "<i2", 2 * n, o).reshape(n, 2); o += 4 * n
    spr_n, glint_n = struct.unpack_from("<2I", b, 64)
    spr = np.frombuffer(b, "<u4", spr_n * spr_n, o).reshape(spr_n, spr_n); o += spr_n * spr_n * 4
    glints = np.frombuffer(b, "<i2", 2 * glint_n, o).reshape(glint_n, 2)
    return dict(w=w, h=h, gem=(gx, gy, gw, gh), word=(wx, wy, ww, wh), ring=(cx, cy, r, n),
                base=base, glow=glow, mask=mask, pts=ring, spr=spr, glints=glints)


def rgb(a):
    return np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], -1).astype(np.int32)


def frame(s, t):
    f = rgb(s["base"]).copy()
    gx, gy, gw, gh = s["gem"]
    p = 150 + SIN[(t * 256 // PULSE_MS) & 255] * 105 // 127
    box = f[gy:gy + gh, gx:gx + gw]
    box[:] = np.minimum(255, box + ((rgb(s["glow"]) * p) >> 8))
    # comet
    cx, cy, r, n = s["ring"]
    head = (t * n // COMET_MS) % n
    for i in range(TAIL - 1, -1, -1):
        dx, dy = s["pts"][(head - i) % n]
        b = 255 - i * 255 // TAIL
        rad = 3 if i < 6 else 2
        x, y = cx + dx, cy + dy
        add = np.array([b * 90 >> 8, b * 200 >> 8, b]) if i >= 4 else np.array([b, b, b])
        sl = f[y - rad:y + rad + 1, x - rad:x + rad + 1]
        sl[:] = np.minimum(255, sl + add)
    # sparkles: one at a time, each for GLINT_MS / count
    k = len(s["glints"])
    ph = t % GLINT_MS
    j = ph * k // GLINT_MS
    slot = GLINT_MS // k
    q = ph - j * slot
    if q < slot:
        a = SIN[(q * 128 // slot) & 255] * 2      # 0..254..0, half a sine over its slot
        sp = rgb(s["spr"])
        hs = sp.shape[0] // 2
        x, y = s["glints"][j]
        sl = f[y - hs:y + hs + 1, x - hs:x + hs + 1]
        sl[:] = np.minimum(255, sl + ((sp * a) >> 8))
    # wordmark sweep
    wx, wy, ww, wh = s["word"]
    ts = t % SWEEP_MS
    if ts < SWEEP_RUN:
        span = ww + wh // 2 + 240
        pos = -120 + ts * span // SWEEP_RUN
        yy, xx = np.mgrid[0:wh, 0:ww]
        k = xx + (wh - yy) // 2
        dd = np.abs(k - pos)
        a = np.where(dd < 70, (70 - dd) * s["mask"].astype(np.int32) // 70, 0)
        wb = f[wy:wy + wh, wx:wx + ww]
        wb[:] = np.minimum(255, wb + a[..., None])
    return f.astype(np.uint8)


def preview(binpath, gifpath, seconds=6, fps=20, scale=0.5):
    s = load(binpath)
    frames = []
    for i in range(seconds * fps):
        im = Image.fromarray(frame(s, i * 1000 // fps))
        frames.append(im.resize((int(s["w"] * scale), int(s["h"] * scale)), Image.LANCZOS))
    frames[0].save(gifpath, save_all=True, append_images=frames[1:], duration=1000 // fps, loop=0, optimize=False)
    print(gifpath, len(frames), "frames")


if __name__ == "__main__":
    preview(sys.argv[1], sys.argv[2])
