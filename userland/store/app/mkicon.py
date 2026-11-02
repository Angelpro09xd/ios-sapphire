#!/usr/bin/env python3
# mkicon.py SIZE OUT.png: the Tienda app icon (a white shopping bag with a down arrow on a blue gradient), plain zlib PNG
import math, struct, sys, zlib


def inside(x, y):
    # coordinates in 0..1; returns True where the icon is white
    # bag body: a rounded rectangle
    bx0, bx1, by0, by1, r = 0.24, 0.76, 0.36, 0.80, 0.07
    cx = min(max(x, bx0 + r), bx1 - r)
    cy = min(max(y, by0 + r), by1 - r)
    body = math.hypot(x - cx, y - cy) <= r
    # handle: an arc above the bag
    d = math.hypot(x - 0.5, y - 0.36)
    handle = y < 0.36 and 0.12 <= d <= 0.165
    # arrow cut out of the body (blue): shaft and head
    shaft = abs(x - 0.5) < 0.04 and 0.45 < y < 0.64
    head = 0.58 <= y <= 0.72 and abs(x - 0.5) < (0.72 - y) * 1.1
    return (body and not (shaft or head)) or handle


def icon(n):
    rows = []
    ss = 3                                     # supersampling for smooth edges
    for y in range(n):
        row = bytearray([0])
        t = y / n
        br, bg, bb = int(40 + 10 * t), int(150 - 60 * t), int(255 - 30 * t)   # blue, darker at the bottom
        for x in range(n):
            k = sum(inside((x + (i + 0.5) / ss) / n, (y + (j + 0.5) / ss) / n) for i in range(ss) for j in range(ss)) / (ss * ss)
            row += bytes((int(br + (255 - br) * k), int(bg + (255 - bg) * k), int(bb + (255 - bb) * k)))
        rows.append(bytes(row))
    raw = b''.join(rows)

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(icon(int(sys.argv[1])))
