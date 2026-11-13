#!/bin/bash
# make-overlay.sh: out/sapphire-overlay.tar.gz from the phone, booted in Android (rooted, the dual-boot layout):
# HfsTar reads include.txt's paths from the iOS partition (read only) minus exclude.txt into a tar on the
# phone, then it comes here. ADB: the Windows adb (WSL has no USB). Nothing is written to iOS.
set -euo pipefail
cd "$(dirname "$0")"
ADB=${ADB:-/mnt/c/adb/adb.exe}
SDK=${ANDROID_HOME:-$HOME/android-sdk}
JAR=$SDK/platforms/android-36/android.jar
D=../../android/dualboot
mkdir -p out out/classes
rm -rf out/classes/*
javac -source 17 -target 17 -nowarn -Xlint:-options -cp $JAR -d out/classes \
    $D/src/com/sapphire/dualboot/{Hfs,RootShell,Root,Ios,L}.java $D/test/HfsTar.java
$SDK/build-tools/36.0.0/d8 --min-api 30 --lib $JAR --output out $(find out/classes -name '*.class')
mv out/classes.dex out/tar.dex
grep -v '^\s*#' include.txt | sed '/^\s*$/d' > out/include.lst
grep -v '^\s*#' exclude.txt | sed '/^\s*$/d' > out/exclude.lst
P=/data/local/tmp/sapphire-overlay
"$ADB" shell "rm -rf $P; mkdir -p $P"
"$ADB" push out/tar.dex out/exclude.lst $P/ >/dev/null
# the tool runs as root (it reads the partition through su)
"$ADB" shell "su -c 'CLASSPATH=$P/tar.dex app_process / com.sapphire.dualboot.HfsTar $P/ov.tar $P/exclude.lst $(tr '\n' ' ' < out/include.lst) 2>$P/log.txt; echo rc=\$?'"
"$ADB" shell "cat $P/log.txt" | tr -d '\r' | tee out/hfstar.log | grep -v '^SKIP' | tail -5
grep -c '^SKIP compressed' out/hfstar.log || true
"$ADB" pull $P/ov.tar out/ov.tar >/dev/null
"$ADB" shell "rm -rf $P"
# checks: no biometric or personal paths, listing
tar -tvf out/ov.tar --numeric-owner > out/overlay-list.txt
if grep -E ' (private/var/root/fp|private/var/root/cam|private/var/mobile|private/var/wireless|private/var/Keychains)(/| |$)' out/overlay-list.txt; then
	echo "personal data in the overlay: stopped" >&2
	exit 1
fi
python3 scrub.py out/ov.tar out/sapphire-overlay.tar.gz   # secrets out (master.passwd hashes)
rm -f out/ov.tar
echo "$(wc -l < out/overlay-list.txt) entries"
ls -la out/sapphire-overlay.tar.gz
bash extras.sh   # the parts built from the repo (Metal bundle, audio driver, Huella)
