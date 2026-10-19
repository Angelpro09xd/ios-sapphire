#!/usr/bin/env python3
"""Patch instructions in a built XNU kernel (Mach-O arm64) in place.

    tools/kpatch.py IN OUT VA=HEX32 [VA=HEX32 ...]

Each VA (kernel virtual address of a 4-byte instruction) gets the given
little-endian instruction word. Used for small fixes when no CI kernel build
is possible; the helpers below encode the few instructions we rewrite.
"""
import struct
import sys

LC_SEGMENT_64 = 0x19


def segments(data):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    off, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, off + 24)
            out.append((name, vmaddr, vmsize, fileoff, filesize))
        off += size
    return out


def va2off(segs, va):
    for name, vmaddr, _, fileoff, filesize in segs:
        if vmaddr <= va < vmaddr + filesize:
            return fileoff + va - vmaddr
    raise ValueError(f'{va:#x} is in no file-backed segment')


def b_cond(src, dst, cond):
    imm = (dst - src) >> 2
    assert -(1 << 18) <= imm < (1 << 18)
    return 0x54000000 | (imm & 0x7ffff) << 5 | cond


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    data = bytearray(open(sys.argv[1], 'rb').read())
    segs = segments(data)
    for arg in sys.argv[3:]:
        va, word = (int(x, 16) for x in arg.split('='))
        off = va2off(segs, va)
        old = struct.unpack_from('<I', data, off)[0]
        struct.pack_into('<I', data, off, word)
        print(f'{va:#x} @ {off:#x}: {old:08x} -> {word:08x}')
    open(sys.argv[2], 'wb').write(data)


if __name__ == '__main__':
    main()
