#!/bin/bash
# xprep.sh: ~/xnu-local/xnu = xnu-8796.141.3 prepared exactly as CI does: darwin-xnu-build at codemagic.yaml's
# DXB_SHA, the repo's xnu/patches copied into its patches/ (so its own patches and ours go in one sorted list),
# its patches() sed edits, then patches/*.sh and patches/*.patch with git apply --check skipping
set -e
L=~/xnu-local
R=$(cd "$(dirname "$0")/../.." && pwd)
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
rm -rf $L/xnu $L/dxb && mkdir -p $L $L/fakeroot
git init -q $L/dxb
git -C $L/dxb fetch -q --depth 1 https://github.com/blacktop/darwin-xnu-build $DXB_SHA
git -C $L/dxb checkout -q FETCH_HEAD
cp $R/xnu/patches/*.patch $R/xnu/patches/*.sh $L/dxb/patches/
git clone -q --no-checkout ~/xnu-src $L/xnu
git -C $L/xnu checkout -q 1b191cb
cd $L/xnu
sed -i 's|^AVAILABILITY_PL="${SDKROOT}/${DRIVERKITROOT}|AVAILABILITY_PL="${FAKEROOT_DIR}|g' bsd/sys/make_symbol_aliasing.sh
sed -i 's|^#include.*BSD.xcconfig.*||g' libsyscall/Libsyscall.xcconfig
sed -i 's|^LDFLAGS_KERNEL_SDK	= -L$(SDKROOT).*|LDFLAGS_KERNEL_SDK	= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel|g' makedefs/MakeInc.def
sed -i 's|^\(INCFLAGS_SDK[[:space:]]*=\) -I$(SDKROOT)/usr/local/include/\(kernel\)\{0,1\}|\1 -I$(FAKEROOT_DIR)/usr/local/include/kernel|' makedefs/MakeInc.def
P=$L/dxb/patches
for s in $P/*.sh; do
	echo "script $(basename $s)"
	MACHINE_CONFIG=SWEET MACOS_VERSION=13.5 KDKROOT= bash $s
done
n=0; skip=0
for p in $P/*.patch; do
	if git apply --check "$p" 2>/dev/null; then git apply "$p"; n=$((n+1)); else echo "SKIP $(basename $p)"; skip=$((skip+1)); fi
done
echo "applied $n, skipped $skip"
grep -c 'sweet_glload' bsd/kern/kern_exec.c || true
grep -rln 'kern_packet_set_fpd' bsd | head -3
