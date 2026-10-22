#!/usr/bin/env python3
"""Check a Mach-O's code signature: the CodeDirectory's page hashes against the file.
  cscheck.py FILE...   -> OK / BAD per file (ad-hoc signatures, SHA-256 or SHA-1)."""
import hashlib, struct, sys
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', d, 0)
    off, cs = 32, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x1d:
            cs = struct.unpack_from('<II', d, off + 8)
        off += size
    if cs is None:
        print('%s: NO SIGNATURE' % path); continue
    so, ss = cs
    m, l, n = struct.unpack_from('>III', d, so)
    ok = None
    for i in range(n):
        t, o = struct.unpack_from('>II', d, so + 12 + 8 * i)
        if t != 0:
            continue
        cd = so + o
        (cmagic, clen, ver, cflags, hoff, idoff, nspecial, ncode, limit, hsize, htype, plat, pshift) = struct.unpack_from('>IIIIIIIIIBBBB', d, cd)
        page = 1 << pshift
        h = hashlib.sha256 if htype == 2 else hashlib.sha1
        bad = 0
        for k in range(ncode):
            want = d[cd + hoff + k * hsize: cd + hoff + (k + 1) * hsize]
            got = h(d[k * page: min((k + 1) * page, limit)]).digest()[:hsize]
            bad += want != got
        ok = bad == 0
        print('%s: %s (%d pages of %d, limit %d, %d bad)' % (path, 'OK' if ok else 'BAD', ncode, page, limit, bad))
    if ok is None:
        print('%s: no CodeDirectory' % path)
