#!/bin/bash
# btshim: Apple's bluetoothd on the sapphire's WCN3988 (research/bluetooth.md). Install as
# /redwood/bin/btshim; jobs: com.sapphire.btshim ("btshim daemon", root) and com.apple.BlueTool
# replaced by "btshim bluetool" (see research/bluetooth.md, "iOS integration").
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
L=${LLVM:-/usr/lib/llvm-18/bin}
mkdir -p ../obj
$L/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter \
	-fblocks -o ../obj/btshim btshim.c
# qcabtfw: the standalone Qualcomm bring-up (firmware download, BD address) for testing by hand
$L/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter 	-framework CoreFoundation -framework IOKit -o ../obj/qcabtfw qcabtfw.c
# btbridge.dylib: inserted into bluetoothd (btshim bdstart), answers its local-mac-address lookup
$L/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter \
	-dynamiclib -install_name /redwood/lib/btbridge.dylib -framework CoreFoundation -framework IOKit \
	-o ../obj/btbridge.dylib btbridge.c
LDID=$(command -v ldid || ls "$HOME"/tools-dl/ldid/ldid 2>/dev/null || true)
for f in btshim qcabtfw btbridge.dylib; do
	[ -n "$LDID" ] && "$LDID" -S ../obj/$f || echo "sign ../obj/$f with ldid -S"
	echo "../obj/$f"
done
