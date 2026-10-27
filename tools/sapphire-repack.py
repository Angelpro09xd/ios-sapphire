#!/usr/bin/env python3
"""Rebuild a UEFI boot image (uefi/mkuefi-v4.py output) around another kernel, keeping the rest.

  sapphire-repack.py OLD.img KERNEL OUT.img [--set ARG=VALUE ...] [--drop ARG ...] [--print]

OLD.img's BootShim + FD, shim, device tree and ADT (with the GPU/IPA firmware mkadt.py put in it)
are reused; only the kernel is replaced (tools/mkpayload.py) and the boot-args edited. For working
without the machine that has the UEFI tree and research/ipsw-x. --print shows OLD.img's boot-args.
Needs MKBOOTIMG (AOSP mkbootimg.py, header v4).
"""
import argparse, os, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('old')
ap.add_argument('kernel', nargs='?')
ap.add_argument('out', nargs='?')
ap.add_argument('--set', action='append', default=[])
ap.add_argument('--drop', action='append', default=[])
ap.add_argument('--print', action='store_true')
a = ap.parse_args()

d = open(a.old, 'rb').read()
assert d[:8] == b'ANDROID!', 'not a boot image'
ks, = struct.unpack_from('<I', d, 8)
raw = d[4096:4096 + ks]
i = -1
while True:
    i = raw.find(b'XNUP', i + 1)
    assert i >= 0, 'no XNUP container'
    total, so, ss, do, ds = struct.unpack_from('<5I', raw, i + 4)
    if so == 0x1000 and total + 8 == do + ds:
        break
fd, c = raw[:i], raw[i:]
shim, dtb = c[so:so + ss], c[do:do + ds]
po = shim.find(b'SWEETXNU')
while po >= 0 and po % 0x1000 != 0:     # (the shim's own code has the magic as a constant)
    po = shim.find(b'SWEETXNU', po + 1)
assert po > 0, 'no payload in the shim'
pl = shim[po:]
adt_off, adt_size = struct.unpack_from('<QQ', pl, 0x20)
cmd = pl[0x48:0x48 + 608].split(b'\0')[0].decode()
if a.print:
    print(cmd)
    sys.exit()

args = cmd.split()
for k in a.drop:
    args = [x for x in args if x.split('=')[0] != k]
for kv in a.set:
    k = kv.split('=')[0]
    args = [x for x in args if x.split('=')[0] != k] + [kv]
cmd = ' '.join(args)

t = tempfile.mkdtemp()
open(os.path.join(t, 'adt'), 'wb').write(pl[adt_off:adt_off + adt_size])
subprocess.check_call([sys.executable, os.path.join(here, 'mkpayload.py'), a.kernel, os.path.join(t, 'adt'),
                       os.path.join(t, 'payload'), '--cmdline', cmd])
shim = bytearray(shim[:po] + open(os.path.join(t, 'payload'), 'rb').read())
struct.pack_into('<Q', shim, 16, len(shim))
dto = (0x1000 + len(shim) + 0xfff) & ~0xfff
total = dto + len(dtb)
assert total <= 0x04000000, 'container larger than the reserved 64 MB'
c = bytearray(total)
struct.pack_into('<6I', c, 0, 0x50554E58, total - 8, 0x1000, len(shim), dto, len(dtb))
c[0x1000:0x1000 + len(shim)] = shim
c[dto:dto + len(dtb)] = dtb
raw = bytearray(fd) + c
struct.pack_into('<QQQ', raw, 8, 0, (len(raw) + 0xfff) & ~0xfff, 0xa)
open(os.path.join(t, 'Image'), 'wb').write(raw)
subprocess.check_call('cd %s && mkdir r && cd r && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin' % t, shell=True)
subprocess.check_call([sys.executable, os.environ['MKBOOTIMG'], '--header_version', '4', '--kernel', os.path.join(t, 'Image'),
                       '--ramdisk', os.path.join(t, 'ramdisk.bin'), '--os_version', '13.0.0', '--os_patch_level', '2025-01',
                       '--cmdline', 'console=null', '-o', a.out])
print('%s: kernel %#x bytes; boot-args: %s' % (a.out, len(raw), cmd))
