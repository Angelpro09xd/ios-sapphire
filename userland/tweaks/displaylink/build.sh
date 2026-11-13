#!/bin/bash
# build.sh OUTDIR: SapphireDisplayLink.dylib (+ .plist) for /var/jb/usr/lib/TweakInject (ElleKit)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
out=${1:-/tmp}
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld=lld -O2 -Wall -fobjc-arc -dynamiclib \
	-install_name /var/jb/usr/lib/TweakInject/SapphireDisplayLink.dylib -lobjc -framework Foundation \
	-o "$out/SapphireDisplayLink.dylib" SapphireDisplayLink.m
"$LDID" -S "$out/SapphireDisplayLink.dylib"
cp SapphireDisplayLink.plist "$out/SapphireDisplayLink.plist"
echo "$out/SapphireDisplayLink.dylib"
