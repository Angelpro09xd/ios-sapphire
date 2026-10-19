#!/usr/bin/env python3
"""Rewrite launchd.plist without some jobs, keeping the exact file size.

    tools/bplist-same-size.py IN OUT LABEL [LABEL ...]

Removes the LaunchDaemons entries whose Label is one of LABEL. The binary
plist is re-encoded (smaller) and padded with zeros between the objects and
the offset table, which the trailer points at, so the new file has the old
length and can be written over the old file's blocks in place (no catalog
change on the HFS+ root).
"""
import plistlib
import struct
import sys

src, dst, labels = sys.argv[1], sys.argv[2], set(sys.argv[3:])
old = open(src, 'rb').read()
plist = plistlib.loads(old)
jobs = plist['LaunchDaemons']
gone = [k for k, j in jobs.items() if j.get('Label') in labels]
assert len(gone) == len(labels), f'found {gone}, wanted {sorted(labels)}'
for k in gone:
    del jobs[k]

new = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY, sort_keys=False)
trailer = bytearray(new[-32:])
off_size, ref_size, nobj, top, table_off = struct.unpack('>6xBBQQQ', trailer)
pad = len(old) - len(new)
assert pad >= 0, 'new plist is larger than the old one'
out = new[:table_off] + bytes(pad) + new[table_off:-32]
struct.pack_into('>Q', trailer, 24, table_off + pad)
out += bytes(trailer)
assert len(out) == len(old)
check = plistlib.loads(out)
assert len(check['LaunchDaemons']) == len(jobs)
open(dst, 'wb').write(out)
print(f'removed {gone}; {len(jobs)} jobs left; {pad} bytes of padding')
