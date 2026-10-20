#!/usr/bin/env python3
"""Replay a pending HFS+ journal (xnu vfs_journal format) offline.

    tools/hfs-journal-replay.py JOURNAL.bin JOURNAL_OFFSET OUT.writes [--check]

JOURNAL.bin is the journal area read from the volume (journal header +
circular buffer, size from the journal info block); JOURNAL_OFFSET is its
byte offset in the volume. Walks the block lists from jhdr->start to
jhdr->end, checking each list header's checksum and sequence number, and
writes OUT.writes: records of (u64 byte offset in the volume, u32 length,
data) in replay order (later records win). With --check only prints. The
journal header with start = end is appended as the last record, so applying
every record replays the journal and marks it empty.
"""
import struct
import sys

JMAGIC, JENDIAN = 0x4a4e4c78, 0x12345678
BLHDR_CHECK_CHECKSUMS = 1


def cksum(data):
    c = 0
    for b in data:
        c = ((c << 8) ^ (c + b)) & 0xffffffff
    return (~c) & 0xffffffff


jpath, joff, out = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
check_only = '--check' in sys.argv
j = open(jpath, 'rb').read()
magic, endian, start, end, size, blhdr_size, jck, jhdr_size, seq = struct.unpack_from('<IIqqqiiiI', j, 0)
assert magic == JMAGIC and endian == JENDIAN, 'not a little-endian journal header'
hdr = bytearray(j[:jhdr_size])
struct.pack_into('<i', hdr, 36, 0)
assert cksum(hdr[:44]) == (jck & 0xffffffff), 'journal header checksum'
assert size == len(j), f'journal size {size} != read {len(j)}'
print(f'journal: start {start} end {end} size {size} blhdr {blhdr_size} dev block {jhdr_size} seq {seq}')


def read(off, n):
    """n bytes of the circular buffer at off (wraps to jhdr_size)."""
    outb = b''
    while n:
        if off >= size:
            off = jhdr_size
        take = min(n, size - off)
        outb += j[off:off + take]
        off += take
        n -= take
    return outb, off


writes = []
off, lists, expect_seq = start, 0, None
while off != end:
    blh, data_off = read(off, blhdr_size)
    max_blocks, num_blocks, bytes_used, ck, flags = struct.unpack_from('<HHiii', blh, 0)
    tmp = bytearray(blh[:32])
    struct.pack_into('<i', tmp, 8, 0)
    if cksum(tmp) != (ck & 0xffffffff):
        raise SystemExit(f'block list at {off}: bad checksum, stopping (nothing written)')
    bseq = struct.unpack_from('<I', blh, 16 + 12)[0]    # binfo[0].b.sequence_num
    if expect_seq is not None and bseq != expect_seq and bseq != expect_seq + 1:
        raise SystemExit(f'block list at {off}: sequence {bseq} after {expect_seq}, stopping')
    expect_seq = bseq
    pos = data_off
    for i in range(1, num_blocks):
        bnum, bsize, bck = struct.unpack_from('<qIi', blh, 16 + 16 * i)
        data, pos = read(pos, bsize)
        if bnum == -1:
            continue                                     # block killed in a later transaction
        if flags & BLHDR_CHECK_CHECKSUMS and cksum(data) != (bck & 0xffffffff):
            raise SystemExit(f'block {bnum} in list at {off}: bad data checksum, stopping')
        writes.append((bnum * jhdr_size, data))
    lists += 1
    nxt = off + bytes_used
    off = nxt if nxt < size else jhdr_size + (nxt - size)

nbytes = sum(len(d) for _, d in writes)
print(f'{lists} block lists, {len(writes)} block writes, {nbytes} bytes, '
      f'{len(set(o for o, _ in writes))} distinct offsets')
if check_only:
    sys.exit(0)
empty = bytearray(j[:jhdr_size])
struct.pack_into('<q', empty, 8, end)                    # start = end
struct.pack_into('<I', empty, 36, 0)
struct.pack_into('<I', empty, 36, cksum(empty[:44]))
writes.append((joff, bytes(empty)))
with open(out, 'wb') as f:
    for o, d in writes:
        f.write(struct.pack('<QI', o, len(d)))
        f.write(d)
print(f'wrote {out}: {len(writes)} records (last = journal header marked empty)')
