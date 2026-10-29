#!/bin/bash
# build.sh [OUTDIR]: snsprobe (the ADSP's sensor core over /dev/redwood-qmiN), for /var/root or /redwood/bin.
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wextra"
out=${1:-../obj}
mkdir -p "$out"
$CC -o "$out/snsprobe" snsprobe.c
"$LDID" -S "$out/snsprobe"
echo "$out/snsprobe"
