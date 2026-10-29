#!/bin/bash
# update.sh [--no-respring] [PHONE]: an installed fingerprint unlock (v1 or v2) brought to this tree's build, from WSL.
#   /redwood/bin/{fpd,fpload,fpsave,fpctl} replaced and the job restarted (launchctl kickstart -k), the SpringBoard and
#   Settings tweaks replaced (SpringBoard restarted unless --no-respring: the phone comes back locked), the
#   Huella app installed / updated (install-app.sh). launchd's cache is not touched; the job's dict in it comes
#   from install.sh (rerun install.sh after changing com.sapphire.fpd.plist).
# The previous binaries are kept on the phone as *.prev (/redwood/bin/fpd.prev, TweakInject/*.dylib.prev):
# fptest.sh rollback-update puts them back.
set -e
cd "$(dirname "$0")"
respring=1
if [ "$1" = --no-respring ]; then respring=0; shift; fi
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
bash build.sh "$out" > /dev/null
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
mkdir -p /redwood/bin /redwood/share/fpd /var/jb/usr/lib/TweakInject /var/tmp/fpd-new
for f in /redwood/bin/fpd /var/jb/usr/lib/TweakInject/SapphireFingerprint.dylib /var/jb/usr/lib/TweakInject/SapphireFingerprintSettings.dylib; do
	[ -f $f ] && /var/jb/bin/cp -p $f $f.prev
done
true
EOF
scp -q $O "$out/fpd" "$out/fpload" "$out/fpsave" "$out/fpctl" "$out/com.sapphire.fpd.plist" "$out/SapphireFingerprint.dylib" \
	"$out/SapphireFingerprint.plist" "$out/SapphireFingerprintSettings.dylib" "$out/SapphireFingerprintSettings.plist" \
	$P:/var/tmp/fpd-new/
ssh $O $P /var/jb/bin/bash -s "$respring" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
N=/var/tmp/fpd-new
T=/var/jb/usr/lib/TweakInject
chown root:wheel $N/*; chmod 755 $N/fpd $N/fpload $N/fpsave $N/fpctl $N/*.dylib; chmod 644 $N/*.plist
/var/jb/bin/mv -f $N/fpd $N/fpload $N/fpsave $N/fpctl /redwood/bin/
/var/jb/bin/mv -f $N/com.sapphire.fpd.plist /redwood/share/fpd/
/var/jb/bin/mv -f $N/SapphireFingerprint.dylib $N/SapphireFingerprint.plist $N/SapphireFingerprintSettings.dylib $N/SapphireFingerprintSettings.plist $T/
rmdir $N 2>/dev/null
if launchctl print system/com.sapphire.fpd >/dev/null 2>&1; then
	launchctl kickstart -k system/com.sapphire.fpd && echo "fpd restarted"
else
	echo "job com.sapphire.fpd not loaded: fptest.sh start"
fi
sleep 3; tail -6 /var/root/fp/fpd.log 2>/dev/null
ls -l /var/run/fpd.sock 2>/dev/null || echo "(no control socket yet)"
killall Preferences 2>/dev/null || true
if [ "$1" = 1 ]; then killall -9 SpringBoard; echo "SpringBoard restarted"; fi
EOF
bash install-app.sh "$out/Huella.ipa" "$P"
