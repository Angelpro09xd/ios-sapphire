#!/bin/bash
# build.sh OUTDIR: fingerprint unlock
#   fpd, fpload, fpsave          -> /redwood/bin/            (fpd: launchd job com.sapphire.fpd)
#   SapphireFingerprint.dylib    -> /var/jb/usr/lib/TweakInject/ (+ .plist: SpringBoard only, ElleKit)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall"
out=${1:-/tmp}
$CC -o "$out/fpd" fpd.c
$CC -o "$out/fpload" fpload.c
$CC -o "$out/fpsave" fpsave.c
$CC -fobjc-arc -dynamiclib -install_name /var/jb/usr/lib/TweakInject/SapphireFingerprint.dylib \
	-lobjc -framework Foundation -framework UIKit -framework QuartzCore \
	-o "$out/SapphireFingerprint.dylib" SapphireFingerprint.m
for f in fpd fpload fpsave SapphireFingerprint.dylib; do "$LDID" -S "$out/$f"; done
cp SapphireFingerprint.plist com.sapphire.fpd.plist "$out/"
echo "$out: fpd fpload fpsave SapphireFingerprint.dylib SapphireFingerprint.plist com.sapphire.fpd.plist"
