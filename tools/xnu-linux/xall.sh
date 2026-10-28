#!/bin/bash
# xall.sh: from scratch, the patched xnu tree, its Linux SETUP tools, headers and libfirehose_kernel (everything
# before the kernel's own make install)
S=$(dirname "$0")
export PATH=~/xnu-tools/bin:$PATH
rm -rf ~/xnu-local/build ~/xnu-local/dst ~/xnu-local/fakeroot/usr/local/lib ~/xnu-local/fakeroot/usr/local/include/kernel
bash $S/xprep.sh 2>&1 | grep -v 'whitespace\|^#  \|^static $\|^ \* $\|^    $\|^/home.*trailing' | tail -6 || exit 1
bash $S/xsetup.sh > /dev/null || exit 1
bash $S/xmake.sh installhdrs || exit 1
bash $S/xfire.sh 2>&1 | grep -v 'warning\|note:\|^ \|^In file\|previous definition' | tail -3
