#!/usr/bin/env python3
# mkicon.py SIZE OUT.png: the Android app icon (a white robot head, half a disc with two antennae and
# eyes, on a green gradient), plain zlib PNG
import math, struct, sys, zlib


def icon(n):
    rows = []
    cx, cy, R = n / 2, n * 0.60, n * 0.30
    for y in range(n):
        row = bytearray([0])
        for x in range(n):
            t = y / n
            r, g, b = int(61 - 20 * t), int(220 - 50 * t), int(132 - 40 * t)
            px, py = x + 0.5, y + 0.5
            d = math.hypot(px - cx, py - cy)
            head = d < R and py < cy and py > cy - R
            eye = any(math.hypot(px - (cx + s * R * 0.42), py - (cy - R * 0.45)) < R * 0.10 for s in (-1, 1))
            antenna = False
            for s in (-1, 1):
                # a short line from the top of the head outwards
                ax0, ay0 = cx + s * R * 0.45, cy - R * 0.80
                ax1, ay1 = cx + s * R * 0.75, cy - R * 1.25
                vx, vy = ax1 - ax0, ay1 - ay0
                u = max(0.0, min(1.0, ((px - ax0) * vx + (py - ay0) * vy) / (vx * vx + vy * vy)))
                if math.hypot(px - (ax0 + u * vx), py - (ay0 + u * vy)) < n * 0.018:
                    antenna = True
            if (head and not eye) or antenna:
                r, g, b = 255, 255, 255
            row += bytes((r, g, b))
        rows.append(bytes(row))
    raw = b''.join(rows)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(icon(int(sys.argv[1])))
