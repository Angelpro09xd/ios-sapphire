#!/bin/bash
# make-update.sh VERSION [OUT_DIR]: a Sapphire update (sapphire-update-VERSION.supdate, a .tar.gz) for the
# installers' update mode. The phone keeps everything (iOS, apps, data); the update brings files and an
# update.sh that iOS runs as root at its next boot (userland/update/sapphire-update.sh), once.
#
# What goes in: everything under files/ (paths as on the phone: files/redwood/bin/x -> /redwood/bin/x), the
# current builds of the per-boot script, the updater and the launchd jobs that run outside launchd's cache
# (files/redwood/launchd/*.plist, loaded by files/redwood/boot.d/*.sh at every boot), and update.sh.
set -e
H=$(cd "$(dirname "$0")" && pwd)
I=$(cd "$H/.." && pwd)
V=${1:-$(cat $I/VERSION)}
OUT=${2:-$I/out}
T=$(mktemp -d)
R=$T/root
mkdir -p $R/redwood/bin $R/redwood/boot.d $R/redwood/launchd $OUT

# the files of this release that live outside the IPSW and change between versions
cp $I/recovery/firstboot.sh $R/redwood/bin/sapphire-firstboot.sh.in
bash $I/../userland/update/build.sh $R/redwood/bin >/dev/null
[ -d $H/files ] && cp -a $H/files/. $R/
# one-off steps (steps.d/NN-*.sh, idempotent, in name order), run by update.sh as root after the files
mkdir -p $T/steps
[ -d $H/steps.d ] && cp $H/steps.d/*.sh $T/steps/ 2>/dev/null
# the app loader (userland/src/glload.c: the GL bridge into apps, tweaks, fast tweak filters)
mkdir -p $R/redwood/lib
SDK=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -dynamiclib \
	-install_name /redwood/lib/libredwood-glload.dylib -framework CoreFoundation -o $R/redwood/lib/libredwood-glload.dylib \
	$I/../userland/src/glload.c
$(find ~/tools-dl/ldid -type f -name ldid | head -1) -S $R/redwood/lib/libredwood-glload.dylib
# the GL bridge (userland/src/eagl.c): backboardd and apps load it; picked up when backboardd restarts (at the boot after)
M=$HOME/mesa-ios/out SDK=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk bash $I/../userland/build-gl.sh >/dev/null
cp $I/../userland/obj/libredwood-gl.dylib $R/redwood/lib/
$(find ~/tools-dl/ldid -type f -name ldid | head -1) -S $R/redwood/lib/libredwood-gl.dylib
# Sapphire's tweaks (ElleKit)
mkdir -p $R/private/var/jb/usr/lib/TweakInject
bash $I/../userland/tweaks/safari/build.sh $R/private/var/jb/usr/lib/TweakInject >/dev/null
LDID=$(find ~/tools-dl/ldid -type f -name ldid | head -1) SDK=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk \
	bash $I/../userland/tweaks/displaylink/build.sh $R/private/var/jb/usr/lib/TweakInject >/dev/null
LDID=$(find ~/tools-dl/ldid -type f -name ldid | head -1) SDK=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk 	bash $I/../userland/tweaks/heif/build.sh $R/private/var/jb/usr/lib/TweakInject >/dev/null
# iOS's dual-boot app (Restart into Android / Update Sapphire): replaces /Applications/Android.app
LDID=$(find ~/tools-dl/ldid -type f -name ldid | head -1) bash $I/../userland/dualboot/build-ios-app.sh >/dev/null
mkdir -p $R/Applications
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' $I/../userland/dualboot/out/Android.ipa $T/ipa
mv $T/ipa/Payload/Android.app $R/Applications/
chmod 755 $R/Applications/Android.app/Android
rm -rf $T/ipa
# launchd jobs added after the install: the recipe's Job dict as a standalone plist, loaded by boot.d
for j in com.sapphire.appwatch; do
	python3 - $I/recipes/launchd/$j.plist $R/redwood/launchd/$j.plist <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
plistlib.dump(d["Job"], open(sys.argv[2], "wb"), fmt=plistlib.FMT_XML)
PY
	cat > $R/redwood/boot.d/50-$j.sh <<EOF
# $j: a launchd job outside launchd's cache, loaded at every boot (make-update.sh)
launchctl list $j >/dev/null 2>&1 || launchctl load /redwood/launchd/$j.plist
EOF
done

cat > $T/update.sh <<'EOF'
#!/var/jb/bin/bash
# update.sh DIR: run by sapphire-update.sh as root, at boot. DIR/root's files go to /.
set -e
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin
D=$1
V=$(cat $D/VERSION)
echo "Sapphire update $V"
# the per-boot script keeps this phone's iOS version and build (filled in at install)
old=/redwood/bin/sapphire-firstboot.sh
ver=$(sed -n 's/^VERSION=\([^ ]*\).*/\1/p' $old 2>/dev/null | head -1)
bld=$(sed -n 's/^BUILD=\([^ ]*\).*/\1/p' $old 2>/dev/null | head -1)
sed "s/@VERSION@/${ver:-16.7.16}/; s/@BUILD@/${bld:-20H392}/" $D/root/redwood/bin/sapphire-firstboot.sh.in > $D/root/redwood/bin/sapphire-firstboot.sh
rm $D/root/redwood/bin/sapphire-firstboot.sh.in
chmod 755 $D/root/redwood/bin/*
chown -R 0:0 $D/root
# an app replaced as a whole (its old files must not linger)
for a in $D/root/Applications/*.app; do
	[ -d "$a" ] && rm -rf "/Applications/${a##*/}"
done
# into /, leaving the metadata of directories that already exist alone (/, /redwood...)
tar -C $D/root -cf - . | tar -C / -xpf - --no-overwrite-dir
echo "$V" > /redwood/version
# what the per-boot steps do, now (the script exits at its once-only part: already done on this phone)
SAPPHIRE_UPDATING=1 bash /redwood/bin/sapphire-firstboot.sh || true
for s in $D/steps/*.sh; do
	[ -f "$s" ] && { echo "step ${s##*/}"; bash "$s" || echo "step ${s##*/} failed"; }
done
# the jobs outside launchd's cache: reloaded, so a changed one takes effect now
for j in /redwood/launchd/*.plist; do
	[ -f "$j" ] && { launchctl unload "$j" 2>/dev/null; launchctl load "$j" || true; }
done
for s in /redwood/boot.d/*.sh; do
	[ -f "$s" ] && bash "$s" || true
done
echo "Sapphire update $V applied"
EOF
echo "$V" > $T/VERSION
chmod 755 $T/update.sh
F=$OUT/sapphire-update-$V.supdate
tar -C $T --owner=0 --group=0 -czf $F update.sh VERSION root steps
rm -rf $T
ls -l $F
tar -tzvf $F | head -30
