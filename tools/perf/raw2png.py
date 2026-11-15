#!/usr/bin/env python3
# raw2png.py RAW PNG W H: BGRA rows of W*4 bytes (fbshot output) to an RGB PNG, standard library only.
import sys, zlib, struct
src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
d = open(src, 'rb').read()
rows = b''.join(b'\0' + bytes(b for i in range(0, w * 4, 4) for b in (r[i + 2], r[i + 1], r[i])) for r in (d[y * w * 4:(y + 1) * w * 4] for y in range(h)))
def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(rows, 6)) + ch(b'IEND', b''))
