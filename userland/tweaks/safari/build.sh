#!/bin/bash
# build.sh OUTDIR: SapphireSafari.dylib (+ .plist) for /var/jb/usr/lib/TweakInject (ElleKit): Safari's container
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$(find $HOME/tools-dl/ldid -type f -name ldid | head -1)}
out=${1:-/tmp}
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld=lld -O2 -Wall -fobjc-arc -dynamiclib \
	-install_name /var/jb/usr/lib/TweakInject/SapphireSafari.dylib -framework Foundation -lobjc \
	-o "$out/SapphireSafari.dylib" SapphireSafari.m
"$LDID" -S "$out/SapphireSafari.dylib"
cp SapphireSafari.plist "$out/SapphireSafari.plist"
echo "$out/SapphireSafari.dylib"
