#!/bin/bash
# build.sh OUTDIR: the Files app fix (see fpfix.c)
#   libsapphire-fpfix.dylib -> /redwood/lib/libsapphire-fpfix.dylib
#   fpstart                 -> /redwood/bin/fpstart, Program of com.apple.FileProvider in /System/Library/xpc/launchd.plist
#   lswrap                  -> .../FileProvider.framework/PlugIns/LocalStorageFileProvider.appex/LocalStorageFileProvider
#                              (Apple's binary kept beside it as LocalStorageFileProvider.pre-files)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall"
out=${1:-/tmp}
$CC -dynamiclib -install_name /redwood/lib/libsapphire-fpfix.dylib -lobjc -lsandbox -o "$out/libsapphire-fpfix.dylib" fpfix.c
$CC -o "$out/fpstart" fpstart.c
$CC -o "$out/lswrap" lswrap.c
for f in libsapphire-fpfix.dylib fpstart lswrap; do "$LDID" -S "$out/$f"; done
echo "$out/libsapphire-fpfix.dylib $out/fpstart $out/lswrap"
