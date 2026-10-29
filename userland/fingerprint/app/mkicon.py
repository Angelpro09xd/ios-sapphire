#!/usr/bin/env python3
# mkicon.py SIZE OUT.png: the Huella app icon (a white fingerprint of arcs on a red gradient), plain zlib PNG
import math, struct, sys, zlib


def icon(n):
    rows = []
    cx, cy = n / 2, n * 0.52
    for y in range(n):
        row = bytearray([0])
        for x in range(n):
            t = y / n
            r, g, b = int(255 - 40 * t), int(59 - 30 * t), int(48 - 20 * t)     # red, darker at the bottom
            dx, dy = (x + 0.5 - cx) / (n * 0.30), (y + 0.5 - cy) / (n * 0.36)
            d = math.hypot(dx, dy)
            ang = math.atan2(dy, dx)
            if d < 1.0:
                ring = d * 5.0                     # five ridges
                frac = ring - math.floor(ring)
                k = int(math.floor(ring))
                # each ridge has a gap at a different angle, the outer ones open at the bottom
                gap = (k * 1.3 + 0.6) % (2 * math.pi) - math.pi
                open_ = abs(math.atan2(math.sin(ang - gap), math.cos(ang - gap))) < 0.35
                bottom = k >= 3 and dy > 0.55
                if 0.30 < frac < 0.72 and not open_ and not bottom and d > 0.08:
                    r, g, b = 255, 255, 255
            row += bytes((r, g, b))
        rows.append(bytes(row))
    raw = b''.join(rows)

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(icon(int(sys.argv[1])))
