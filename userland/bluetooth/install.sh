#!/bin/bash
# install.sh [--fake] [--no-start] [PHONE]: Bluetooth onto the phone (root@192.168.7.2, key ~/.ssh/sapphire_phone),
# from WSL; rerun = update or switch mode.
#   /redwood/bin/btshim, /redwood/bin/qcabtfw, firmware /var/root/btfw/apbtfw11.tlv + apnv11.bin (if missing; from
#   ~/ios-build/bt/fw/img/image), and three jobs in launchd's cache (bt-launchd.py; the cache before the first
#   install stays on the phone as launchd.plist.pre-btshim, every replaced cache also in ~/ios-build/launchd-backups):
#     com.sapphire.btshim   btshim daemon (root, KeepAlive, jetsam 18): bring-up once the boot is 120 s old, then
#                           the H4 bridge on /var/run/btshim/hci-{in,out}
#     com.apple.bluetoothd  back in the cache, with BT_HCI_TRANSPORT=VIRT:VIRT:/var/run/btshim/hci, BT_DEVICE_ADDRESS;
#                           program /redwood/bin/btshim bdstart (execs it with /redwood/lib/btbridge.dylib inserted)
#     com.apple.BlueTool    -> /redwood/bin/btshim bluetool
#   --fake: recording mode, no hardware (btshim daemon -F -v -w 0): bluetoothd talks to a fake BCM4357B1 and every
#           packet goes to /var/tmp/btshim.log (record.sh pulls it).
#   BT_DEVICE_ADDRESS = qcabtfw -A on the phone (static random, from IOPlatformUUID; BT_ADDR=.. overrides).
# BTAudioHALPlugin stays in /System/Library/Audio/Plug-Ins/HAL.disabled (mediaserverd is not touched).
# Then (unless --no-start) the three jobs are (re)started now: btshim, BlueTool, bluetoothd. A reboot also works.
set -e
cd "$(dirname "$0")"
fake=0 start=1
while [ $# -gt 0 ]; do
	case "$1" in
	--fake) fake=1; shift ;;
	--no-start) start=0; shift ;;
	*) break ;;
	esac
done
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
ORIG=${ORIG:-$HOME/ios-build/bt/System/Library/LaunchDaemons}
FW=${FW:-$HOME/ios-build/bt/fw/img/image}
for f in "$ORIG/com.apple.bluetoothd.plist" "$ORIG/com.apple.BlueTool.plist"; do
	[ -f "$f" ] || { echo "missing $f (7z x ~/ipsw/087-86698-044.dmg System/Library/LaunchDaemons/...)"; exit 1; }
done
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
bash build.sh > /dev/null
cp ../obj/btshim ../obj/qcabtfw ../obj/btbridge.dylib "$out/"

ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
mkdir -p /redwood/bin /redwood/lib /redwood/share/btshim /var/root/btfw /var/tmp/btshim-new
L=/System/Library/xpc/launchd.plist
[ -f $L.pre-btshim ] || /var/jb/bin/cp -p $L $L.pre-btshim
for f in /redwood/bin/btshim /redwood/bin/qcabtfw /redwood/lib/btbridge.dylib; do [ -f $f ] && /var/jb/bin/cp -p $f $f.prev; done
true
EOF
scp -q $O "$out/btshim" "$out/qcabtfw" "$out/btbridge.dylib" $P:/var/tmp/btshim-new/
for f in apbtfw11.tlv apnv11.bin; do
	if ! ssh $O $P "test -s /var/root/btfw/$f"; then
		[ -f "$FW/$f" ] || { echo "missing $FW/$f"; exit 1; }
		scp -q $O "$FW/$f" $P:/var/root/btfw/ && echo "firmware $f copied"
	fi
done
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
N=/var/tmp/btshim-new
chown root:wheel $N/*; chmod 755 $N/*
/var/jb/bin/mv -f $N/btshim $N/qcabtfw /redwood/bin/
/var/jb/bin/mv -f $N/btbridge.dylib /redwood/lib/
rmdir $N 2>/dev/null; true
EOF
addr=${BT_ADDR:-$(ssh $O $P /redwood/bin/qcabtfw -A | tr -d '\r')}
echo "BD address $addr"

bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-btshim-install.$stamp"
python3 -I bt-launchd.py install "$bk/launchd.plist.before-btshim-install.$stamp" "$out/launchd.plist" \
	--orig "$ORIG" --job com.sapphire.btshim.plist --addr "$addr" $( [ $fake = 1 ] && echo --fake ) --jobs-out "$out/jobs"
scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.btshim-new
scp -q $O "$out"/jobs/*.plist $P:/redwood/share/btshim/
sum=$(sha256sum "$out/launchd.plist" | cut -d' ' -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" "$start" "$fake" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
got=$(shasum -a 256 $L.btshim-new 2>/dev/null | cut -d' ' -f1)
[ -z "$got" ] && got=$(sha256sum $L.btshim-new | cut -d' ' -f1)
if [ "$got" != "$1" ]; then echo "launchd.plist upload corrupt, cache NOT replaced"; rm -f $L.btshim-new; exit 1; fi
chown root:wheel $L.btshim-new; chmod 644 $L.btshim-new
/var/jb/bin/mv -f $L.btshim-new $L && echo "launchd's cache: btshim, bluetoothd, BlueTool jobs written"
chown root:wheel /redwood/share/btshim/*.plist; chmod 644 /redwood/share/btshim/*.plist
ls /System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver >/dev/null 2>&1 && echo "WARNING: BTAudioHALPlugin is back in HAL/ (keep it in HAL.disabled for now)"
launchctl print-disabled system 2>/dev/null | grep -i -E 'bluetooth|BlueTool' | grep -i true && launchctl enable system/com.apple.bluetoothd
[ "$2" = 1 ] || { echo "not started (--no-start): reboot to apply"; exit 0; }
# stop the old ones (bluetoothd first: it holds the FIFOs and talks to BlueTool)
for j in com.apple.bluetoothd com.apple.BlueTool com.sapphire.btshim; do
	launchctl bootout system/$j 2>/dev/null || launchctl remove $j 2>/dev/null || true
done
killall -9 bluetoothd BlueTool btshim 2>/dev/null; sleep 1
for j in com.sapphire.btshim com.apple.BlueTool com.apple.bluetoothd; do
	if launchctl bootstrap system /redwood/share/btshim/$j.plist 2>/dev/null || launchctl load /redwood/share/btshim/$j.plist 2>/dev/null; then
		echo "$j loaded"
	else
		echo "$j: launchctl refused; reboot to apply"
	fi
	[ $j = com.sapphire.btshim ] && sleep 2
done
if [ "$3" = 1 ]; then w=10; else w=60; fi
echo "waiting ${w} s ..."; sleep $w
pgrep -l -f 'btshim|bluetoothd' ; tail -15 /var/tmp/btshim.log 2>/dev/null
EOF
echo "log: ssh $P tail -f /var/tmp/btshim.log   (record.sh pulls it)"
