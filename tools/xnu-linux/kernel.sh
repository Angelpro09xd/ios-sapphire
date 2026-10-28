#!/bin/bash
# kernel.sh OUT.img [BASE.img]: the phone's kernel from xnu/patches, built from scratch on Linux, repacked into a
# copy of BASE.img (default /mnt/c/adb/sapphire-uefi-4k-0241-glload.img: its shim, DTB, ADT and boot-args: 2x at
# 750 wide, and sweet_glload=1 so that SpringBoard starts with the tweaks)
set -e
S=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$S/../.." && pwd)
OUT=$1
BASE=${2:-/mnt/c/adb/sapphire-uefi-4k-0241-glload.img}
K=~/xnu-local/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
[ -n "$OUT" ] || { echo "usage: kernel.sh OUT.img [BASE.img]"; exit 1; }
bash $S/xall.sh
bash $S/xcckext.sh
rm -f $K
# make install stops at the DriverKit exports check (iig-lite, see README) after the kernel is linked
bash $S/xmake.sh install || true
[ -s $K ] || { echo "no kernel: see ~/xnu-local/install.log"; exit 1; }
strings $K | grep -m1 'Darwin Kernel Version'
MKBOOTIMG=${MKBOOTIMG:-$HOME/ios-build/fox/mkbootimg.py} python3 $R/tools/sapphire-repack.py "$BASE" $K "$OUT"
ls -la "$OUT"
