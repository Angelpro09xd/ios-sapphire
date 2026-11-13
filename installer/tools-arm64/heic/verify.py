"""Verify PNGs produced by sapphire-heic2png against the HEIC sources.

usage: python3 -I verify.py TESTOUT_DIR TESTDATA_DIR
For each testdata/**.HEIC: runs the x86 tool with --info, opens the matching
PNG with PIL, checks dimensions / mode / alpha, prints a table line.
"""
import os, subprocess, sys, zlib, struct
from PIL import Image

out_dir, data_dir = sys.argv[1], sys.argv[2]
tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'sapphire-heic2png.x86')
fail = 0
rows = []
for root, _, files in sorted(os.walk(data_dir)):
    for fn in sorted(files):
        if not fn.lower().endswith('.heic'):
            continue
        src = os.path.join(root, fn)
        rel = os.path.relpath(src, data_dir)
        png = os.path.join(out_dir, os.path.splitext(rel)[0] + '.png')
        info = subprocess.run([tool, '--info', src], capture_output=True, text=True, check=True).stdout.splitlines()
        dims, alpha = info[0].split()
        w, h = map(int, dims.split('x'))
        # raw chunk walk: every CRC must match, collect chunk types
        data = open(png, 'rb').read()
        assert data[:8] == b'\x89PNG\r\n\x1a\n'
        p, chunks = 8, []
        while p < len(data):
            ln, = struct.unpack('>I', data[p:p+4]); typ = data[p+4:p+8]
            crc, = struct.unpack('>I', data[p+8+ln:p+12+ln])
            if zlib.crc32(data[p+4:p+8+ln]) != crc:
                raise SystemExit(f'CRC mismatch in {png} chunk {typ}')
            chunks.append(typ.decode()); p += 12 + ln
        Image.open(png).verify()
        im = Image.open(png); im.load()
        ok = im.size == (w, h) and im.mode == ('RGBA' if alpha == 'alpha' else 'RGB')
        extra = ''
        if im.mode == 'RGBA':
            a = im.getchannel('A')
            lo, hi = a.getextrema()
            hist = a.histogram()
            extra = f'alpha min={lo} max={hi} transparent={hist[0]*100/(w*h):.1f}% opaque={hist[255]*100/(w*h):.1f}%'
        rgb_ext = im.convert('RGB').getextrema()
        fail |= not ok
        rows.append((rel, os.path.getsize(src), os.path.getsize(png), im.mode, f'{w}x{h}', 'OK' if ok else 'FAIL',
                     ','.join(c for c in dict.fromkeys(chunks)), extra, 'rgb_extrema=' + str(rgb_ext), info[1:]))
for r in rows:
    print(f'{r[0]}\n    heic={r[1]} B  png={r[2]} B  {r[3]}  {r[4]}  {r[5]}  chunks={r[6]}\n    {r[7]} {r[8]}')
    for l in r[9]:
        print('    info: ' + l)
print('RESULT:', 'FAIL' if fail else 'ALL OK')
sys.exit(1 if fail else 0)
