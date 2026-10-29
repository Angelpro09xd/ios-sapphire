#!/bin/bash
# build.sh OUTDIR: fingerprint unlock, v2
#   fpd, fpload, fpsave, fpctl                    -> /redwood/bin/                (fpd: launchd job com.sapphire.fpd)
#   SapphireFingerprint.dylib (+ .plist)     -> /var/jb/usr/lib/TweakInject/ (SpringBoard: lock-screen glyph, unlock)
#   SapphireFingerprintSettings.dylib (+ .plist) -> same                     (Settings: the "Huella dactilar" row)
#   Huella.ipa (Payload/Huella.app)          -> TrollStore                   (the management app)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CC="/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall"
out=${1:-/tmp}
$CC -framework CoreFoundation -o "$out/fpd" fpd.c
$CC -o "$out/fpload" fpload.c
$CC -o "$out/fpsave" fpsave.c
$CC -o "$out/fpctl" fpctl.c
for t in SapphireFingerprint SapphireFingerprintSettings; do
	$CC -fobjc-arc -dynamiclib -install_name /var/jb/usr/lib/TweakInject/$t.dylib \
		-lobjc -framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics -o "$out/$t.dylib" $t.m
	cp $t.plist "$out/"
done
for f in fpd fpload fpsave fpctl SapphireFingerprint.dylib SapphireFingerprintSettings.dylib; do "$LDID" -S "$out/$f"; done
cp com.sapphire.fpd.plist "$out/"

# the app
app=$(mktemp -d)
A="$app/Payload/Huella.app"
mkdir -p "$A"
$CC -fobjc-arc -framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics -o "$A/Huella" app/Huella.m
"$LDID" -Sapp/Huella.entitlements "$A/Huella"
cp app/Info.plist "$A/Info.plist"
python3 -I app/mkicon.py 120 "$A/AppIcon60x60@2x.png"
python3 -I app/mkicon.py 180 "$A/AppIcon60x60@3x.png"
rm -f "$out/Huella.ipa"
python3 -I - "$app" "$out/Huella.ipa" <<'EOF'
import os, sys, zipfile
root, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, 'Payload')):
        for f in sorted(files):
            p = os.path.join(d, f)
            info = zipfile.ZipInfo(os.path.relpath(p, root))
            info.external_attr = (0o100755 if f == 'Huella' else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, open(p, 'rb').read())
EOF
rm -rf "$app"
echo "$out: fpd fpload fpsave SapphireFingerprint.dylib SapphireFingerprintSettings.dylib (+ plists) com.sapphire.fpd.plist Huella.ipa"
