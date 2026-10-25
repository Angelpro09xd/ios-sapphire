#!/bin/bash
# qmitool (QMI to the modem over /dev/redwood-qmiN, kernel 0220). Copy to the phone, e.g.
# /var/jb/usr/bin/qmitool, and run it as root.
set -e
cd "$(dirname "$0")"
SDK=${SDK:-/serverhive/angelpro09devv/sdks/theos-sdks/iPhoneOS16.5.sdk}
L=${LLVM:-/usr/lib/llvm-18/bin}
mkdir -p ../obj
$L/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter \
	-o ../obj/qmitool qmitool.c
echo "../obj/qmitool"
# ccbridge.dylib: loaded into CommCenter (DYLD_INSERT_LIBRARIES), installed as /redwood/lib/ccbridge.dylib
$L/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter \
	-dynamiclib -install_name /redwood/lib/ccbridge.dylib -framework CoreFoundation -framework IOKit -o ../obj/ccbridge.dylib ccbridge.c
echo "../obj/ccbridge.dylib"
