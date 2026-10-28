#!/bin/bash
# mkcct.sh: the rest of cctools the xnu build calls (codesign_allocate, libtool, lipo, nm, nmedit, ctf_insert)
cd ~/xnu-tools/native-ld
cmake build -DCMAKE_EXE_LINKER_FLAGS=-lm > /dev/null
for t in codesign_allocate libtool lipo nm nmedit ctf_insert; do
	ninja -C build host_$t > ninja-$t.log 2>&1 || { echo "== $t FAILED"; grep -m 8 -e 'error' -e 'undefined' ninja-$t.log; continue; }
	f=build/tools/cctools/misc/$t
	[ -f "$f" ] || f=$(find build -type f -name "$t" | grep -v CMakeFiles | head -1)
	cp "$f" ~/xnu-tools/bin/ && echo "$t ok ($f)"
done
ls ~/xnu-tools/bin
