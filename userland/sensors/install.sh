#!/bin/bash
# install.sh [PHONE]: the sensors onto the phone (root@192.168.7.2, key ~/.ssh/sapphire_phone), from WSL; rerun = update.
#   /redwood/bin/{sensord,snsprobe,sensload}, /redwood/share/sensord/com.sapphire.sensord.plist
#   /var/root/sensors/files/: what the ADSP's sensors PD reads (mkfiles.sh: Lunaris vendor/etc/sensors, a read-only
#     copy of the persist backup's sensors/ registry and calibration, sys/devices/soc0); the previous tree is kept
#     as files.prev
#   the job com.sapphire.sensord in launchd's cache (../haptics/launchd-job.py add; the cache before the first install
#     stays on the phone as launchd.plist.pre-sensord, every replaced cache also in ~/ios-build/launchd-backups/)
# Then sensord is (re)started now. It needs kernel 0268 (kern.sweet_sensor; 0265-0267 for FastRPC).
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
bash build.sh "$out" > /dev/null
bash mkfiles.sh "$out/sensfiles.tar.gz"
cp com.sapphire.sensord.plist "$out/"
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
mkdir -p /redwood/bin /redwood/share/sensord /var/root/sensors /var/tmp/sensord-new
L=/System/Library/xpc/launchd.plist
[ -f $L.pre-sensord ] || /var/jb/bin/cp -p $L $L.pre-sensord
true
EOF
scp -q -l 8000 $O "$out/sensord" "$out/snsprobe" "$out/sensload" "$out/com.sapphire.sensord.plist" "$out/sensfiles.tar.gz" \
	$P:/var/tmp/sensord-new/

bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q -l 8000 $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-sensord-install.$stamp"
newcache=0
if ! python3 -I -c "import plistlib,sys;sys.exit(0 if '/System/Library/LaunchDaemons/com.sapphire.sensord.plist' in plistlib.load(open(sys.argv[1],'rb'))['LaunchDaemons'] else 1)" \
	"$bk/launchd.plist.before-sensord-install.$stamp"; then
	python3 -I ../haptics/launchd-job.py add com.sapphire.sensord.plist "$bk/launchd.plist.before-sensord-install.$stamp" \
		"$out/launchd.plist"
	scp -q -l 8000 $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.sensord-new
	newcache=1
fi
sum=$( [ $newcache = 1 ] && sha256sum "$out/launchd.plist" | cut -d' ' -f1 || echo none)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
if [ "$1" != none ]; then
	got=$(shasum -a 256 $L.sensord-new 2>/dev/null | cut -d' ' -f1)
	[ -z "$got" ] && got=$(sha256sum $L.sensord-new | cut -d' ' -f1)
	if [ "$got" != "$1" ]; then echo "launchd.plist upload corrupt, cache NOT replaced"; rm -f $L.sensord-new; exit 1; fi
	chown root:wheel $L.sensord-new; chmod 644 $L.sensord-new
	/var/jb/bin/mv -f $L.sensord-new $L && echo "job com.sapphire.sensord added to launchd's cache"
fi
N=/var/tmp/sensord-new
chown root:wheel $N/*; chmod 755 $N/sensord $N/snsprobe $N/sensload; chmod 644 $N/*.plist
/var/jb/bin/mv -f $N/sensord $N/snsprobe $N/sensload /redwood/bin/
/var/jb/bin/mv -f $N/com.sapphire.sensord.plist /redwood/share/sensord/
S=/var/root/sensors
if [ -d $S/files ]; then /var/jb/bin/rm -rf $S/files.prev; /var/jb/bin/mv $S/files $S/files.prev; fi
mkdir -p $S/files && tar -C $S/files -xzf $N/sensfiles.tar.gz && echo "$(find $S/files -type f | wc -l) files in $S/files"
/var/jb/bin/rm -f $N/sensfiles.tar.gz; rmdir $N 2>/dev/null
if launchctl print system/com.sapphire.sensord >/dev/null 2>&1; then
	launchctl kickstart -k system/com.sapphire.sensord && echo "sensord restarted"
elif launchctl bootstrap system /redwood/share/sensord/com.sapphire.sensord.plist 2>/dev/null || \
	launchctl load /redwood/share/sensord/com.sapphire.sensord.plist 2>/dev/null; then
	echo "sensord loaded"
else
	nohup /redwood/bin/sensord >> /var/tmp/sensord.out 2>&1 & echo "sensord started by hand (launchctl refused)"
fi
sleep 3; pgrep -l sensord; tail -5 /var/root/sensors/sensord.log 2>/dev/null
EOF
