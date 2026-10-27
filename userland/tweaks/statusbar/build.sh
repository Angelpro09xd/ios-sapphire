#!/bin/bash
# build.sh OUTDIR: SapphireStatusBar.dylib (+ .plist) for /var/jb/usr/lib/TweakInject (ElleKit)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
out=${1:-/tmp}
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld=lld -O2 -Wall -dynamiclib \
	-install_name /var/jb/usr/lib/TweakInject/SapphireStatusBar.dylib -lobjc -o "$out/SapphireStatusBar.dylib" SapphireStatusBar.c
"$LDID" -S "$out/SapphireStatusBar.dylib"
cp SapphireStatusBar.plist "$out/SapphireStatusBar.plist"
echo "$out/SapphireStatusBar.dylib"
