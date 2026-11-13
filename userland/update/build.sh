#!/bin/bash
# build.sh OUTDIR: the iOS side of updates -> /redwood/bin/
#   sapphire-inboxcat    reads the inbox through kern.sweet_inbox (kernel 0310)
#   sapphire-update.sh   applies its *.supdate entries (run by sapphire-firstboot.sh at every boot)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$(find $HOME/tools-dl/ldid -type f -name ldid | head -1)}
out=${1:-/tmp}
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -o "$out/sapphire-inboxcat" inboxcat.c
"$LDID" -S "$out/sapphire-inboxcat"
cp sapphire-update.sh sapphire-appwatch.sh "$out/"
chmod 755 "$out/sapphire-update.sh" "$out/sapphire-appwatch.sh"
ls -l "$out/sapphire-inboxcat" "$out/sapphire-update.sh"
