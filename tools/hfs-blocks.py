import struct, sys
img, before, after, prev = sys.argv[1:5]
b = open(before, 'rb').read(); a = open(after, 'rb').read()
new = []
for i in range(len(a)):
    x = a[i] & ~b[i]
    if x:
        for bit in range(8):
            if x & (0x80 >> bit):
                new.append(i * 8 + bit)
freed = sum(bin(b[i] & ~a[i]).count('1') for i in range(len(a)))
with open(img, 'rb') as f:
    v = f.read(1536)[1024:]
total = struct.unpack_from('>I', v, 44)[0]; jib = struct.unpack_from('>I', v, 12)[0]
meta = [(0, 1), (jib, 2), (total - 1, 1)]
for i in range(5):
    o = 112 + 80 * i
    tot = struct.unpack_from('>I', v, o + 12)[0]
    ext = [struct.unpack_from('>II', v, o + 16 + 8 * k) for k in range(8)]
    ext = [e for e in ext if e[1]]
    assert sum(e[1] for e in ext) == tot
    meta += ext
meta += [tuple(map(int, l.split())) for l in open(prev)]
blocks = set(new)
for s, c in meta:
    blocks.update(range(s, s + c))
blocks = sorted(blocks)
runs = []
for x in blocks:
    if runs and runs[-1][0] + runs[-1][1] == x:
        runs[-1][1] += 1
    else:
        runs.append([x, 1])
print(f'nuevos {len(new)} bloques ({len(new)*4/1024:.1f} MB), liberados {freed}, total a escribir {len(blocks)} bloques ({len(blocks)*4/1024:.1f} MB) en {len(runs)} tramos', file=sys.stderr)
for s, c in runs:
    print(s, c)
