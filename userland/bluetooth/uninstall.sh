#!/bin/bash
# uninstall.sh [--rollback] [PHONE]: Bluetooth off the phone, from WSL.
#   default:    surgical (bt-launchd.py uninstall on the current cache): com.sapphire.btshim removed, com.apple.bluetoothd
#               removed again (as the image build has it), com.apple.BlueTool back to Apple's /usr/sbin/BlueTool -R;
#               every other job kept as it is now.
#   --rollback: the whole cache back to launchd.plist.pre-btshim (the cache before the first install). Jobs added by
#               other installers since then are lost, so prefer the default.
# The running jobs are stopped and Apple's BlueTool job reloaded; /redwood/bin/btshim and /redwood/share/btshim removed.
# qcabtfw and /var/root/btfw stay (tools). The replaced cache is kept in ~/ios-build/launchd-backups.
set -e
cd "$(dirname "$0")"
rollback=0
[ "$1" = --rollback ] && { rollback=1; shift; }
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
ORIG=${ORIG:-$HOME/ios-build/bt/System/Library/LaunchDaemons}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-btshim-uninstall.$stamp"
if [ $rollback = 1 ]; then
	scp -q $O $P:/System/Library/xpc/launchd.plist.pre-btshim "$out/launchd.plist"
	echo "rollback: launchd.plist.pre-btshim"
else
	python3 -I bt-launchd.py uninstall "$bk/launchd.plist.before-btshim-uninstall.$stamp" "$out/launchd.plist" --orig "$ORIG"
fi
scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.btshim-new
sum=$(sha256sum "$out/launchd.plist" | cut -d' ' -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
got=$(shasum -a 256 $L.btshim-new 2>/dev/null | cut -d' ' -f1)
[ -z "$got" ] && got=$(sha256sum $L.btshim-new | cut -d' ' -f1)
if [ "$got" != "$1" ]; then echo "upload corrupt, cache NOT replaced"; rm -f $L.btshim-new; exit 1; fi
chown root:wheel $L.btshim-new; chmod 644 $L.btshim-new
/var/jb/bin/mv -f $L.btshim-new $L && echo "launchd's cache replaced"
for j in com.apple.bluetoothd com.apple.BlueTool com.sapphire.btshim; do
	launchctl bootout system/$j 2>/dev/null || launchctl remove $j 2>/dev/null || true
done
killall -9 bluetoothd btshim 2>/dev/null
launchctl bootstrap system /System/Library/LaunchDaemons/com.apple.BlueTool.plist 2>/dev/null \
	&& echo "Apple's BlueTool job reloaded" || echo "BlueTool: reboot to reload Apple's job"
rm -f /redwood/bin/btshim /redwood/bin/btshim.prev /redwood/lib/btbridge.dylib*; rm -rf /redwood/share/btshim /var/run/btshim
echo "uninstalled (qcabtfw and /var/root/btfw kept; the WCN stays powered until reboot)"
EOF
