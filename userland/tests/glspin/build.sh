#!/bin/bash
# build.sh OUTDIR: GLSpin.app (ad-hoc signed) into OUTDIR. On the phone: /Applications/GLSpin.app, then
# `uicache -p /Applications/GLSpin.app` and open it from the home screen (or uiopen).
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
out=${1:-/tmp}/GLSpin.app
rm -rf "$out" && mkdir -p "$out"
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld=lld -O2 -fobjc-arc -Wall \
	-Wno-deprecated-declarations -framework UIKit -framework QuartzCore -framework OpenGLES -framework Foundation \
	-o "$out/GLSpin" main.m
cp Info.plist "$out/Info.plist"
"$LDID" -S "$out/GLSpin"
echo "$out"
