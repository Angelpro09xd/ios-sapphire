#!/bin/bash
# build.sh OUTDIR: vibration
#   vibd                              -> /redwood/bin/vibd (launchd job com.sapphire.vibd)
#   SapphireHaptics.dylib (+ .plist)  -> /var/jb/usr/lib/TweakInject/ (UIKit apps + SpringBoard)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall"
out=${1:-/tmp}
$CC -o "$out/vibd" vibd.c
$CC -fobjc-arc -dynamiclib -install_name /var/jb/usr/lib/TweakInject/SapphireHaptics.dylib \
	-lobjc -framework Foundation -framework UIKit -framework CoreGraphics -o "$out/SapphireHaptics.dylib" SapphireHaptics.m
for f in vibd SapphireHaptics.dylib; do "$LDID" -S "$out/$f"; done
cp SapphireHaptics.plist com.sapphire.vibd.plist "$out/"
echo "$out: vibd SapphireHaptics.dylib SapphireHaptics.plist com.sapphire.vibd.plist"
