"""Dump an HFS+ volume's allocation bitmap out of an image: bm.py IMG OUT."""
import struct
import sys

img, out = sys.argv[1], sys.argv[2]
with open(img, 'rb') as f:
    f.seek(1024)
    v = f.read(512)
    assert v[:2] == b'HX', v[:2]
    o = 112                                     # allocationFile fork data
    total = struct.unpack_from('>I', v, o + 12)[0]
    ext = [struct.unpack_from('>II', v, o + 16 + 8 * k) for k in range(8)]
    ext = [e for e in ext if e[1]]
    assert sum(e[1] for e in ext) == total, (ext, total)
    with open(out, 'wb') as g:
        for start, count in ext:
            f.seek(start * 4096)
            g.write(f.read(count * 4096))
print('bitmap', total, 'bloques de', ext)
