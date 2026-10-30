#!/usr/bin/env python3
"""dngcheck.py OUT.dng [BAYER.pgm]: parse rawtool's DNG with the standard library only,
print the tags and, given the --bayer dump of the same run, check the pixel data."""
import struct
import sys

NAMES = {254: 'NewSubfileType', 256: 'ImageWidth', 257: 'ImageLength', 258: 'BitsPerSample',
         259: 'Compression', 262: 'Photometric', 270: 'ImageDescription', 271: 'Make', 272: 'Model',
         273: 'StripOffsets', 274: 'Orientation', 277: 'SamplesPerPixel', 278: 'RowsPerStrip',
         279: 'StripByteCounts', 284: 'PlanarConfig', 305: 'Software', 33421: 'CFARepeatPatternDim',
         33422: 'CFAPattern', 50706: 'DNGVersion', 50707: 'DNGBackwardVersion', 50708: 'UniqueCameraModel',
         50710: 'CFAPlaneColor', 50711: 'CFALayout', 50713: 'BlackLevelRepeatDim', 50714: 'BlackLevel',
         50717: 'WhiteLevel', 50721: 'ColorMatrix1', 50728: 'AsShotNeutral', 50778: 'CalibrationIlluminant1'}
SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 10: 8}


def tags(d):
    assert d[:4] == b'II*\0', 'not a little-endian TIFF'
    off = struct.unpack_from('<I', d, 4)[0]
    n = struct.unpack_from('<H', d, off)[0]
    out, prev = {}, 0
    for i in range(n):
        t, ty, c, v = struct.unpack_from('<HHII', d, off + 2 + 12 * i)
        assert t > prev, 'tags not sorted'
        prev = t
        b = SIZE[ty] * c
        p = off + 2 + 12 * i + 8 if b <= 4 else v
        raw = d[p:p + b]
        if ty == 2:
            val = raw.rstrip(b'\0').decode()
        elif ty == 3:
            val = list(struct.unpack('<%dH' % c, raw))
        elif ty == 4:
            val = list(struct.unpack('<%dI' % c, raw))
        elif ty == 1:
            val = list(raw)
        else:
            q = struct.unpack('<%d%s' % (2 * c, 'I' if ty == 5 else 'i'), raw)
            val = [round(q[2 * k] / q[2 * k + 1], 4) for k in range(c)]
        out[t] = val
    return out


def main():
    d = open(sys.argv[1], 'rb').read()
    t = tags(d)
    for k in (256, 257, 258, 262, 271, 272, 33422, 50706, 50714, 50717, 50721, 50728):
        print('  %-20s %s' % (NAMES[k], t[k]))
    for k in (254, 256, 257, 258, 259, 262, 273, 277, 278, 279, 33421, 33422, 50706, 50714, 50717, 50721):
        assert k in t, 'missing tag %s' % NAMES[k]
    so, sc = t[273], t[279]
    W, H = t[256][0], t[257][0]
    assert sum(sc) == W * H * 2 and so[-1] + sc[-1] <= len(d), 'strip layout'
    if len(sys.argv) > 2:
        pg = open(sys.argv[2], 'rb').read()
        hdr = pg.index(b'65535\n') + 6
        bl, wl = t[50714][0], t[50717][0]
        rps = t[278][0]
        worst = 0
        for y in range(0, H, max(1, H // 97)):
            s = y // rps
            row = so[s] + (y % rps) * W * 2
            for x in range(0, W, max(1, W // 89)):
                dv = struct.unpack_from('<H', d, row + 2 * x)[0]
                pv = struct.unpack_from('>H', pg, hdr + 2 * (y * W + x))[0]
                if pv == 0 and dv <= bl:
                    continue  # below black: kept in the DNG, clamped in the dump
                worst = max(worst, abs(dv - (bl + pv / 65535 * (wl - bl))))
        print('  pixel data vs --bayer dump: max diff %.2f (16-bit codes)' % worst)
        assert worst < 2, 'pixel data mismatch'
    print('dng: ok')


main()
