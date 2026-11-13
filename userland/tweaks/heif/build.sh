#!/bin/bash
# build.sh OUTDIR: SapphireHEIF.dylib (+ .plist) for /var/jb/usr/lib/TweakInject (ElleKit): HEIF decoded in software
# (libheif + libde265 from ../../heif/build-libs.sh, built in)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$(find $HOME/tools-dl/ldid -type f -name ldid | head -1)}
P=../../heif/prefix
[ -f $P/lib/libheif.a ] || bash ../../heif/build-libs.sh >/dev/null
out=${1:-/tmp}
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld=lld -O2 -Wall -fobjc-arc -dynamiclib 	-install_name /var/jb/usr/lib/TweakInject/SapphireHEIF.dylib -I$P/include -DLIBHEIF_STATIC_BUILD 	-framework Foundation -framework ImageIO -framework CoreGraphics -lobjc 	-o "$out/SapphireHEIF.dylib" SapphireHEIF.m $P/lib/libheif.a $P/lib/libde265.a -lc++
"$LDID" -S "$out/SapphireHEIF.dylib"
cp SapphireHEIF.plist "$out/SapphireHEIF.plist"
echo "$out/SapphireHEIF.dylib"
