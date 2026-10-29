#!/bin/bash
# uninstall.sh [PHONE]: vibration off the phone, from WSL: vibd stopped and its job taken out of launchd's cache
# (launchd-job.py remove; every other job kept as it is now), /redwood/bin/vibd and the SapphireHaptics tweak removed,
# SpringBoard restarted. (launchd.plist.pre-vibd on the phone is the cache from before the first install.)
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
launchctl bootout system/com.sapphire.vibd 2>/dev/null || launchctl remove com.sapphire.vibd 2>/dev/null || true
killall vibd 2>/dev/null || true
rm -f /var/jb/usr/lib/TweakInject/SapphireHaptics.dylib* /var/jb/usr/lib/TweakInject/SapphireHaptics.plist
true
EOF
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-vibd-uninstall.$stamp"
python3 -I launchd-job.py remove com.sapphire.vibd.plist "$bk/launchd.plist.before-vibd-uninstall.$stamp" "$out/launchd.plist"
scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.vibd-new
sum=$(sha256sum "$out/launchd.plist" | cut -d' ' -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
got=$(shasum -a 256 $L.vibd-new 2>/dev/null | cut -d' ' -f1)
[ -z "$got" ] && got=$(sha256sum $L.vibd-new | cut -d' ' -f1)
if [ "$got" != "$1" ]; then echo "upload corrupt, cache NOT replaced"; rm -f $L.vibd-new; exit 1; fi
chown root:wheel $L.vibd-new; chmod 644 $L.vibd-new
/var/jb/bin/mv -f $L.vibd-new $L && echo "job removed from launchd's cache"
rm -f /redwood/bin/vibd /redwood/bin/vibd.prev; rm -rf /redwood/share/vibd
killall -9 SpringBoard; echo "uninstalled (SpringBoard restarted)"
EOF
