#!/bin/bash
# build.sh [OUTDIR]: Tienda.ipa, the app store (AltStore-format sources, installs through TrollStore)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall"
out=${1:-/tmp}
app=$(mktemp -d)
A="$app/Payload/Tienda.app"
mkdir -p "$A"
$CC -fobjc-arc -framework Foundation -framework UIKit -framework CoreGraphics -o "$A/Tienda" app/Tienda.m
"$LDID" -Sapp/Tienda.entitlements "$A/Tienda"
cp app/Info.plist "$A/Info.plist"
python3 -I app/mkicon.py 120 "$A/AppIcon60x60@2x.png"
python3 -I app/mkicon.py 180 "$A/AppIcon60x60@3x.png"
rm -f "$out/Tienda.ipa"
python3 -I - "$app" "$out/Tienda.ipa" <<'EOF'
import os, sys, zipfile
root, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, 'Payload')):
        for f in sorted(files):
            p = os.path.join(d, f)
            info = zipfile.ZipInfo(os.path.relpath(p, root))
            info.external_attr = (0o100755 if f == 'Tienda' else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, open(p, 'rb').read())
EOF
rm -rf "$app"
echo "$out/Tienda.ipa"
