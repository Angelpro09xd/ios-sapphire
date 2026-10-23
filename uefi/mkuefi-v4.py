#!/usr/bin/env python3
# Mu-Silicium's boot image (v1, gzip(BootShim + FD) + DTB) -> a boot image sapphire's ABL boots:
# header v4 (the DTB comes from vendor_boot), the raw BootShim + FD as the kernel, with the ARM64
# header of our own shim: text_offset 0, flags 0xa (4K pages, anywhere), image_size the file's.
# BootShim finds the FD relative to itself and copies it to FD_BASE, so the load address is free.
#   mkuefi-v4.py Mu-<device>.img out.img [SHIM_IMAGE DTB]
# With SHIM_IMAGE and DTB, an "XNUP" container follows the FD (BootShim copies it to PAYLOAD_BASE,
# SapphireXnuBoot.c starts the shim from it): u32 'XNUP', u32 size after these 8, u32 shim offset,
# u32 shim size, u32 dtb offset, u32 dtb size; the shim at 0x1000 (its payload rides 4K aligned
# behind it), the device tree on the next 4K boundary.
import struct, subprocess, sys, tempfile, zlib, os
src, out = sys.argv[1], sys.argv[2]
shim_path = sys.argv[3] if len(sys.argv) > 4 else None
dtb_path = sys.argv[4] if len(sys.argv) > 4 else None
d = open(src, 'rb').read()
ks, = struct.unpack_from('<I', d, 8)
ps, = struct.unpack_from('<I', d, 36)
k = d[ps:ps + ks]
z = zlib.decompressobj(16 + zlib.MAX_WBITS)
raw = bytearray(z.decompress(k))
assert raw[56:60] == b'ARMd', 'no ARM64 header'
if shim_path:
    shim = open(shim_path, 'rb').read()
    # /chosen/sapphire,quiet = 1: the shim draws nothing (the screen keeps the UEFI's logo)
    qd = os.path.join(tempfile.mkdtemp(), 'quiet.dtb')
    open(qd, 'wb').write(open(dtb_path, 'rb').read())
    subprocess.check_call(['fdtput', '-t', 'u', qd, '/chosen', 'sapphire,quiet', '1'])
    dtb = open(qd, 'rb').read()
    assert shim[56:60] == b'ARMd', 'the shim is no arm64 Image'
    shim_off = 0x1000
    dtb_off = (shim_off + len(shim) + 0xfff) & ~0xfff
    total = dtb_off + len(dtb)
    c = bytearray(total)
    struct.pack_into('<6I', c, 0, 0x50554E58, total - 8, shim_off, len(shim), dtb_off, len(dtb))
    c[shim_off:shim_off + len(shim)] = shim
    c[dtb_off:dtb_off + len(dtb)] = dtb
    assert total <= 0x04000000, 'container larger than the reserved 64 MB'
    raw += c
    print('XNUP: shim %#x bytes at %#x, dtb %#x bytes at %#x, %#x in all' % (len(shim), shim_off, len(dtb), dtb_off, total))
struct.pack_into('<QQQ', raw, 8, 0, (len(raw) + 0xfff) & ~0xfff, 0xa)
tmp = tempfile.mkdtemp()
kern = os.path.join(tmp, 'Image'); open(kern, 'wb').write(raw)
rd = os.path.join(tmp, 'ramdisk.bin')
subprocess.check_call('cd %s && mkdir r && cd r && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin' % tmp, shell=True)
subprocess.check_call([sys.executable, '/serverhive/angelpro09devv/infinity/system/tools/mkbootimg/mkbootimg.py',
    '--header_version', '4', '--kernel', kern, '--ramdisk', rd, '--os_version', '13.0.0',
    '--os_patch_level', '2025-01', '--cmdline', 'console=null', '-o', out])
print('%s: kernel %#x bytes' % (out, len(raw)))
