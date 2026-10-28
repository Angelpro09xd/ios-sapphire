#!/bin/bash
# xsetup.sh: xnu's SETUP host tools as PureDarwin ported them to Linux (decomment, installfile, json_compilation_db,
# kextsymboltool, replacecontents, setsegname, mach_compat.h); our config/ (newer: bound-checks) stays
set -e
PDX=~/tools-dl/pd/PureDarwin/src/Kernel/xnu/SETUP
X=~/xnu-local/xnu/SETUP
cp $PDX/mach_compat.h $X/
for t in decomment installfile json_compilation_db kextsymboltool replacecontents setsegname; do
	rm -rf $X/$t && cp -r $PDX/$t $X/$t
done
diff -u ~/xnu-src/SETUP/config/Makefile $PDX/config/Makefile | head -40
