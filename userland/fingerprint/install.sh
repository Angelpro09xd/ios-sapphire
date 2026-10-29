#!/bin/bash
# install.sh [PHONE]: fingerprint unlock onto the phone (root@192.168.7.2, key ~/.ssh/sapphire_phone), from WSL.
#   /redwood/bin/{fpd,fpload,fpsave}, /redwood/share/fpd/com.sapphire.fpd.plist
#   /var/jb/usr/lib/TweakInject/SapphireFingerprint.{dylib,plist} (loaded by the next SpringBoard)
#   launchd's cache /System/Library/xpc/launchd.plist + job com.sapphire.fpd (fpd-launchd.py add); the cache
#   before the first install stays on the phone as launchd.plist.pre-fpd, and every cache this script replaces
#   is also kept in ~/ios-build/launchd-backups/ here.
# Nothing is started: fptest.sh start (or a reboot) starts fpd, fptest.sh respring loads the tweak.
# Rollback: uninstall.sh.
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
bash build.sh "$out" > /dev/null
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
mkdir -p /redwood/bin /redwood/share/fpd /var/jb/usr/lib/TweakInject
[ -d /var/root/fp ] || echo "WARNING: /var/root/fp missing (fpd will do nothing)"
L=/System/Library/xpc/launchd.plist
[ -f $L.pre-fpd ] || /var/jb/bin/cp -p $L $L.pre-fpd
ls -l $L $L.pre-fpd
EOF
scp -q $O "$out/fpd" "$out/fpload" "$out/fpsave" $P:/redwood/bin/
scp -q $O "$out/com.sapphire.fpd.plist" $P:/redwood/share/fpd/
scp -q $O "$out/SapphireFingerprint.dylib" "$out/SapphireFingerprint.plist" $P:/var/jb/usr/lib/TweakInject/

# the launchd cache, patched here (python), checked, then swapped in on the phone
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-fpd-install.$stamp"
python3 -I fpd-launchd.py add "$bk/launchd.plist.before-fpd-install.$stamp" "$out/launchd.plist"
scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.fpd-new
sum=$(sha256sum "$out/launchd.plist" | cut -d' ' -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
got=$(shasum -a 256 $L.fpd-new 2>/dev/null | cut -d' ' -f1)
[ -z "$got" ] && got=$(sha256sum $L.fpd-new | cut -d' ' -f1)
if [ "$got" != "$1" ]; then echo "launchd.plist upload corrupt ($got), cache NOT replaced"; rm -f $L.fpd-new; exit 1; fi
chown root:wheel $L.fpd-new; chmod 644 $L.fpd-new
/var/jb/bin/mv -f $L.fpd-new $L
chown root:wheel /redwood/bin/fpd /redwood/bin/fpload /redwood/bin/fpsave /redwood/share/fpd/com.sapphire.fpd.plist
chmod 755 /redwood/bin/fpd /redwood/bin/fpload /redwood/bin/fpsave
chmod 644 /redwood/share/fpd/com.sapphire.fpd.plist /var/jb/usr/lib/TweakInject/SapphireFingerprint.plist
chmod 755 /var/jb/usr/lib/TweakInject/SapphireFingerprint.dylib
ls -l $L /redwood/bin/fpd /var/jb/usr/lib/TweakInject/SapphireFingerprint.*
echo "installed; start: fptest.sh start, tweak: fptest.sh respring"
EOF
