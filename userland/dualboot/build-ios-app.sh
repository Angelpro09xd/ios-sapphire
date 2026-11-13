#!/bin/bash
# build-ios-app.sh: the iOS side of the dual boot, out/Android.ipa (Payload/Android.app): one button that
# makes Android's A/B slot active (xnu 0305, kern.sweet_boot_android) and restarts into Android.
# Installed like the Huella app: ../fingerprint/install-app.sh-style, /Applications + uicache.
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall"
mkdir -p out
app=$(mktemp -d)
A="$app/Payload/Android.app"
mkdir -p "$A"
$CC -fobjc-arc -framework Foundation -framework UIKit -o "$A/Android" ios-app/AndroidBoot.m
"$LDID" -Sios-app/AndroidBoot.entitlements "$A/Android"
cp ios-app/Info.plist "$A/Info.plist"
python3 -I ios-app/mkicon.py 120 "$A/AppIcon60x60@2x.png"
python3 -I ios-app/mkicon.py 180 "$A/AppIcon60x60@3x.png"
python3 -I - "$app" "$PWD/out/Android.ipa" <<'EOF'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
EOF
rm -rf "$app"
ls -la out/Android.ipa
