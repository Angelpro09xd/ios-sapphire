#!/usr/bin/env python3
"""C table entries for files a kernel driver reads straight from a partition.

    tools/fwtable-file.py PARTITION [--base OFFSET] FILE [FILE ...]

Each FILE is a path inside a mounted filesystem (the partition itself, or an
image of it mounted on a loop device). The tool asks the kernel where the file's
blocks are (FIEMAP, or FIBMAP as root when the filesystem has no FIEMAP, as
HFS+ here), checks that they are one contiguous run, and prints

    { "name", "PARTITION", 0xoffset, size, 0xcrc32 },

where offset is the byte offset of the file inside PARTITION: what
sweet_ufs_read_part() takes. --base is added to every offset, for a filesystem
that does not start at the beginning of the partition (a logical partition
inside `super`, for instance).

Fragmented, compressed or inline files are reported and skipped: the driver's
table holds one plain run per file. Copying such a file somewhere it lands
contiguously is the way around it.
"""
import fcntl
import os
import struct
import sys
import zlib

FS_IOC_FIEMAP = 0xC020660B
FIEMAP_FLAG_SYNC = 1
FIEMAP_EXTENT_LAST = 0x0001
FIEMAP_EXTENT_UNKNOWN = 0x0002
FIEMAP_EXTENT_DELALLOC = 0x0004
FIEMAP_EXTENT_ENCODED = 0x0008
FIEMAP_EXTENT_DATA_INLINE = 0x0200
BAD = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_INLINE
FIBMAP = 1
BLOCK = 4096


def fiemap(fd, size):
    """[(physical offset, length)] of the file, or None if FIEMAP is unavailable."""
    count = 64
    buf = bytearray(32 + 56 * count)
    struct.pack_into('<QQIIII', buf, 0, 0, size, FIEMAP_FLAG_SYNC, 0, count, 0)
    try:
        fcntl.ioctl(fd, FS_IOC_FIEMAP, buf)
    except OSError:
        return None
    mapped = struct.unpack_from('<I', buf, 20)[0]
    out = []
    for i in range(mapped):
        logical, physical, length = struct.unpack_from('<QQQ', buf, 32 + 56 * i)
        flags = struct.unpack_from('<I', buf, 32 + 56 * i + 40)[0]
        if flags & BAD:
            raise SystemExit(f'extent {i} has flags {flags:#x} (compressed, inline or not on disk)')
        out.append((physical, length))
    return out


def fibmap(fd, size):
    """The same through the older per-block call, which needs root."""
    out = []
    for i in range((size + BLOCK - 1) // BLOCK):
        try:
            b = struct.unpack('I', fcntl.ioctl(fd, FIBMAP, struct.pack('I', i)))[0]
        except OSError as e:
            raise SystemExit(f'FIBMAP failed ({e}); run as root')
        if b == 0:
            raise SystemExit(f'block {i} is a hole or not mapped')
        if out and out[-1][0] + out[-1][1] == b * BLOCK:
            out[-1] = (out[-1][0], out[-1][1] + BLOCK)
        else:
            out.append((b * BLOCK, BLOCK))
    return out


def extents(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        size = os.fstat(fd).st_size
        ext = fiemap(fd, size)
        if ext is None:
            ext = fibmap(fd, size)
        merged = []
        for off, length in ext:
            if merged and merged[-1][0] + merged[-1][1] == off:
                merged[-1] = (merged[-1][0], merged[-1][1] + length)
            else:
                merged.append((off, length))
        return merged, size
    finally:
        os.close(fd)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    part = args.pop(0)
    base = 0
    if args and args[0] == '--base':
        args.pop(0)
        base = int(args.pop(0), 0)
    rc = 0
    for path in args:
        try:
            ext, size = extents(path)
        except SystemExit as e:
            print(f'# {os.path.basename(path)}: {e}', file=sys.stderr)
            rc = 1
            continue
        if len(ext) != 1:
            print(f'# {os.path.basename(path)}: {len(ext)} runs, not contiguous; copy it and try again',
                  file=sys.stderr)
            rc = 1
            continue
        crc = zlib.crc32(open(path, 'rb').read())
        print(f'\t{{ "{os.path.basename(path)}", "{part}", {base + ext[0][0]:#x}, {size}, {crc:#010x} }},')
    return rc


if __name__ == '__main__':
    sys.exit(main())
