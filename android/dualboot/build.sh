#!/bin/bash
# build.sh: Sapphire Dual Boot (out/SapphireDualBoot.apk) with the bare SDK tools: aapt2, javac, d8, zipalign,
# apksigner. No Gradle. ANDROID_HOME defaults to ~/android-sdk; the signing key is made once in ~/.android.
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-$HOME/android-sdk}
BT=$SDK/build-tools/36.0.0
JAR=$SDK/platforms/android-36/android.jar
KS=$HOME/.android/sapphire-dualboot.jks
rm -rf out; mkdir -p out/classes
# the icon (art/mkicon.py: the Android-Apple logo filled with the boot screen's picture, on a glass tile)
python3 art/mkicon.py art/logo-android-apple.jpg ../../shim/splash/splash.png art/out >/dev/null
rm -rf res; cp -r art/out/res res
# what is not generated (the Quick Settings tile's icon) goes on top
cp -r res-extra/. res/
$BT/aapt2 compile --dir res -o out/res.zip
$BT/aapt2 link -o out/base.apk --manifest AndroidManifest.xml -I $JAR -R out/res.zip --auto-add-overlay
javac -source 17 -target 17 -nowarn -Xlint:-options -cp $JAR -d out/classes $(find src -name '*.java')
$BT/d8 --min-api 30 --lib $JAR --output out $(find out/classes -name '*.class')
# bootctl: Magisk's (lib/arm64-v8a/libbootctl.so of its APK, GPL-3), the Android boot HAL client the switch to iOS uses
BOOTCTL=${BOOTCTL:-/mnt/c/Users/angel/AppData/Local/Temp/claude/38097bab8b376548/537925f0-f11c-4102-af71-fd6c497e0f6e/scratchpad/magisk-dl/x/lib/arm64-v8a/libbootctl.so}
[ -f "$BOOTCTL" ] || { echo "BOOTCTL=path to Magisk's libbootctl.so (unzip it from Magisk-*.apk)"; exit 1; }
(cd out && cp base.apk unsigned.apk && python3 -c "
import sys, zipfile
z = zipfile.ZipFile('unsigned.apk', 'a')
z.write('classes.dex', 'classes.dex')
zi = zipfile.ZipInfo('lib/arm64-v8a/libbootctl.so', (2026, 1, 1, 0, 0, 0)); zi.external_attr = 0o755 << 16
z.writestr(zi, open(sys.argv[1], 'rb').read(), compress_type=zipfile.ZIP_STORED)
z.close()" "$BOOTCTL")
$BT/zipalign -f -p 4 out/unsigned.apk out/aligned.apk
if [ ! -f $KS ]; then
	mkdir -p $(dirname $KS)
	keytool -genkeypair -keystore $KS -storepass sapphire -keypass sapphire -alias dualboot -keyalg RSA -keysize 2048 \
	    -validity 10000 -dname "CN=Sapphire Dual Boot" >/dev/null 2>&1
fi
$BT/apksigner sign --ks $KS --ks-pass pass:sapphire --out out/SapphireDualBoot.apk out/aligned.apk
ls -la out/SapphireDualBoot.apk
