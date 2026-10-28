#!/bin/bash
# xcckext.sh: libcc_kext.a (cc_kext.c) into the fakeroot, where xmake.sh's LD_KERNEL_LIBS points
set -e
S=$(cd "$(dirname "$0")" && pwd)
L=~/xnu-local
export PATH=~/xnu-tools/bin:$PATH
mkdir -p $L/build/cckext $L/fakeroot/usr/local/lib/kernel
xclang -arch arm64 -mkernel -nostdinc -I $L/xnu/EXTERNAL_HEADERS -O2 -c $S/cc_kext.c -o $L/build/cckext/cc_kext.o
rm -f $L/fakeroot/usr/local/lib/kernel/libcc_kext.a
libtool -static -o $L/fakeroot/usr/local/lib/kernel/libcc_kext.a $L/build/cckext/cc_kext.o
