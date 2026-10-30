#!/bin/bash
# uninstall.sh [--rollback] [PHONE]: from WSL.
#   (default)   camd stopped (camera powered off with op 9), its job taken out of launchd's cache
#               (launchd-job.py remove; every other job kept as it is now), /redwood/bin/{camd,camctl} and
#               /redwood/share/camd removed. Photos in Files/Camara and /var/root/camd.conf stay.
#   --rollback  the previous camd/camctl (*.prev from the last install.sh) back in place and restarted.
# (launchd.plist.pre-camd on the phone is the cache from before the first install.)
set -e
cd "$(dirname "$0")"
rollback=0
if [ "$1" = --rollback ]; then rollback=1; shift; fi
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
if [ $rollback = 1 ]; then
	ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
[ -f /redwood/bin/camd.prev ] || { echo "no /redwood/bin/camd.prev"; exit 1; }
/var/jb/bin/cp -p /redwood/bin/camd.prev /redwood/bin/camd
[ -f /redwood/bin/camctl.prev ] && /var/jb/bin/cp -p /redwood/bin/camctl.prev /redwood/bin/camctl
launchctl kickstart -k system/com.sapphire.camd && echo "previous camd restarted"
EOF
	exit 0
fi
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/sbin:/var/jb/usr/sbin:/usr/bin:/bin
launchctl bootout system/com.sapphire.camd 2>/dev/null || launchctl remove com.sapphire.camd 2>/dev/null || true
killall camd 2>/dev/null || true
sysctl -w kern.sweet_cam=9 >/dev/null 2>&1 || true
rm -f /var/run/camd.sock /var/tmp/camd-preview.bin /var/tmp/camd-raw.bin
true
EOF
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-camd-uninstall.$stamp"
python3 -I ../../haptics/launchd-job.py remove com.sapphire.camd.plist "$bk/launchd.plist.before-camd-uninstall.$stamp" "$out/launchd.plist"
scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.camd-new
sum=$(sha256sum "$out/launchd.plist" | cut -d' ' -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
got=$(shasum -a 256 $L.camd-new 2>/dev/null | cut -d' ' -f1)
[ -z "$got" ] && got=$(sha256sum $L.camd-new | cut -d' ' -f1)
if [ "$got" != "$1" ]; then echo "upload corrupt, cache NOT replaced"; rm -f $L.camd-new; exit 1; fi
chown root:wheel $L.camd-new; chmod 644 $L.camd-new
/var/jb/bin/mv -f $L.camd-new $L && echo "job removed from launchd's cache"
rm -f /redwood/bin/camd /redwood/bin/camd.prev /redwood/bin/camctl /redwood/bin/camctl.prev; rm -rf /redwood/share/camd
echo "camd uninstalled"
EOF
