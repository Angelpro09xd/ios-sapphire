#!/usr/bin/env python3
"""Split a turnip FD_RD_DUMP=full capture into its buffers and command streams.

  rdbos.py CAPTURE.rd OUTDIR      -> OUTDIR/<va>.bin per buffer, OUTDIR/index.txt
"""
import gzip, os, struct, sys

d = open(sys.argv[1], 'rb').read()
if d[:2] == b'\x1f\x8b':
    d = gzip.decompress(d)
out = sys.argv[2]; os.makedirs(out, exist_ok=True)
o, cur, idx = 0, None, []
while o < len(d):
    t, s = struct.unpack_from('<II', d, o); o += 8; p = d[o:o + s]; o += s
    if t == 3:
        lo, sz, hi = struct.unpack('<III', p); cur = (hi << 32 | lo, sz); idx.append('bo %x %x' % cur)
    elif t == 12:
        open(os.path.join(out, '%x.bin' % cur[0]), 'wb').write(p)
    elif t == 6:
        lo, sz, hi = struct.unpack('<III', p); idx.append('cmd %x %u' % (hi << 32 | lo, sz))
open(os.path.join(out, 'index.txt'), 'w').write('\n'.join(idx) + '\n')
print('\n'.join(idx))
