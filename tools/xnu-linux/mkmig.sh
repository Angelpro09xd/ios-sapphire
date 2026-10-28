#!/bin/bash
# mkmig.sh: migcom for Linux (PureDarwin's nix/pkgs/toolchain/migcom.nix without Nix) and the mig driver script,
# into ~/xnu-tools/bin. Mach headers from our iPhoneOS 16.5 SDK (arm/ only: it has no i386/).
set -e
PD=~/tools-dl/pd/PureDarwin
W=~/xnu-tools/mig
SDK=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk
rm -rf $W && cp -r $PD/tools/mig $W && cd $W
yacc -d -b y parser.y
lex -o lexxer.yy.c lexxer.l
mkdir -p mach_shim/sys
cp -r "$SDK/usr/include/mach" mach_shim/mach
cp -r "$SDK/usr/include/arm" mach_shim/arm
chmod -R u+w mach_shim
sed -i -e "/typedef long long *__int64_t;/d" -e "/typedef unsigned long long *__uint64_t;/d" \
	-e "/^typedef union {/,/} __mbstate_t;/d" mach_shim/arm/_types.h
cp "$SDK/usr/include/sys/_types.h" "$SDK/usr/include/sys/_endian.h" "$SDK/usr/include/sys/appleapiopts.h" mach_shim/sys/
cp -r "$SDK/usr/include/sys/_types" mach_shim/sys/_types
cp -r "$SDK/usr/include/sys/_pthread" mach_shim/sys/_pthread
cp -r "$SDK/usr/include/machine" mach_shim/machine
cp -r "$SDK/usr/include/architecture" mach_shim/architecture 2>/dev/null || true
cp -r "$SDK/usr/include/_types" mach_shim/_types
cp -r "$SDK/usr/include/libkern" mach_shim/libkern
cp "$SDK/usr/include/Availability.h" "$SDK/usr/include/AvailabilityInternal.h" "$SDK/usr/include/AvailabilityVersions.h" \
	"$SDK/usr/include/AvailabilityMacros.h" mach_shim/
mkdir -p obj
objs=""
for src in error.c global.c header.c mig.c routine.c server.c statement.c string.c type.c user.c utils.c y.tab.c lexxer.yy.c; do
	o=obj/$(basename $src .c).o
	clang-18 -std=gnu89 -w -D__private_extern__= -D__LITTLE_ENDIAN__ -DMIG_VERSION=\"\" -I . -I mach_shim \
		-include sys/types.h -include bits/types/__mbstate_t.h -D__arm64__=1 \
		-D__kernel_ptr_semantics= -D__kernel_data_semantics= -D__kernel_dual_semantics= -c $src -o $o
	objs="$objs $o"
done
clang-18 -o ~/xnu-tools/bin/migcom $objs
cp mig.sh ~/xnu-tools/bin/mig && chmod +x ~/xnu-tools/bin/mig
echo "built: $(ls -la ~/xnu-tools/bin/migcom)"
