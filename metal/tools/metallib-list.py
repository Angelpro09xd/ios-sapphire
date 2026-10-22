#!/usr/bin/env python3
"""List the functions in a .metallib (MTLB): name, type, bitcode size.

    metallib-list.py FILE.metallib [--dump-bitcode DIR]

MTLB layout (as the public reverse engineering of the format describes it, checked against the
file): "MTLB", versions, u64 file size at 16, then (u64 offset, u64 size) pairs from 24 for the
function list, public metadata, private metadata and bitcode. The function list is a u32 count
and, per function, a u32 size followed by tags (4-char name, u16 length, data) up to ENDT:
NAME (C string), TYPE (u8), HASH (sha256), MDSZ (u64 bitcode size), OFFT (u64 public, private
and bitcode offsets, relative to their sections), ...
With --dump-bitcode each function's bitcode (AIR, an LLVM bitcode wrapper) is written to DIR.
"""
import os
import struct
import sys

TYPES = {0: 'vertex', 1: 'fragment', 2: 'kernel', 3: 'unqualified', 4: 'visible', 5: 'extern',
         6: 'intersection', 7: 'mesh', 8: 'object'}

d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'MTLB', d[:4]
size = struct.unpack_from('<Q', d, 16)[0]
fl_off, fl_size, pub_off, pub_size, prv_off, prv_size, bc_off, bc_size = struct.unpack_from('<8Q', d, 24)
dump = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == '--dump-bitcode' else None
if dump:
    os.makedirs(dump, exist_ok=True)
count = struct.unpack_from('<I', d, fl_off)[0]
p = fl_off + 4
print(f'# {sys.argv[1]}: {size} bytes, {count} functions, bitcode {bc_size} bytes')
for i in range(count):
    gsize = struct.unpack_from('<I', d, p)[0]
    q, end = p + 4, p + gsize
    f = {}
    while q < end:
        tag = d[q:q + 4].decode('latin-1')
        if tag == 'ENDT':
            break
        ln = struct.unpack_from('<H', d, q + 4)[0]
        f[tag] = d[q + 6:q + 6 + ln]
        q += 6 + ln
    name = f.get('NAME', b'').rstrip(b'\0').decode()
    typ = TYPES.get(f.get('TYPE', b'\xff')[0], '?')
    mdsz = struct.unpack_from('<Q', f['MDSZ'])[0] if 'MDSZ' in f else 0
    print(f'{typ:12s} {mdsz:8d}  {name}')
    if dump and 'OFFT' in f:
        boff = struct.unpack_from('<3Q', f['OFFT'])[2]
        open(os.path.join(dump, name + '.air.bc'), 'wb').write(d[bc_off + boff:bc_off + boff + mdsz])
    p = end
