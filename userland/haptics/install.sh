#!/bin/bash
# install.sh [PHONE]: vibration onto the phone (root@192.168.7.2, key ~/.ssh/sapphire_phone), from WSL; rerun = update.
#   /redwood/bin/vibd (+ /redwood/share/vibd/com.sapphire.vibd.plist), the job com.sapphire.vibd in launchd's cache
#   (launchd-job.py add; the cache before the first install kept on the phone as launchd.plist.pre-vibd, every
#   replaced cache also in ~/ios-build/launchd-backups/ here), /var/jb/usr/lib/TweakInject/SapphireHaptics.*.
# Then vibd is (re)started now (kickstart, or bootstrap of the plist) and SpringBoard restarted (the tweak loads into
# apps as they start; --no-respring to skip). The previous vibd / tweak stay as *.prev (vibtest.sh rollback).
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
mkdir -p /redwood/bin /redwood/share/vibd /var/jb/usr/lib/TweakInject /var/tmp/vibd-new
L=/System/Library/xpc/launchd.plist
[ -f $L.pre-vibd ] || /var/jb/bin/cp -p $L $L.pre-vibd
for f in /redwood/bin/vibd /var/jb/usr/lib/TweakInject/SapphireHaptics.dylib; do [ -f $f ] && /var/jb/bin/cp -p $f $f.prev; done
true
EOF
scp -q $O "$out/vibd" "$out/com.sapphire.vibd.plist" "$out/SapphireHaptics.dylib" "$out/SapphireHaptics.plist" $P:/var/tmp/vibd-new/

# the launchd cache: only if the job is not in it yet
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-vibd-install.$stamp"
newcache=0
if ! python3 -I -c "import plistlib,sys;sys.exit(0 if '/System/Library/LaunchDaemons/com.sapphire.vibd.plist' in plistlib.load(open(sys.argv[1],'rb'))['LaunchDaemons'] else 1)" \
	"$bk/launchd.plist.before-vibd-install.$stamp"; then
	python3 -I launchd-job.py add com.sapphire.vibd.plist "$bk/launchd.plist.before-vibd-install.$stamp" "$out/launchd.plist"
	scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.vibd-new
	newcache=1
fi
sum=$( [ $newcache = 1 ] && sha256sum "$out/launchd.plist" | cut -d' ' -f1 || echo none)
ssh $O $P /var/jb/bin/bash -s "$sum" "$respring" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
if [ "$1" != none ]; then
	got=$(shasum -a 256 $L.vibd-new 2>/dev/null | cut -d' ' -f1)
	[ -z "$got" ] && got=$(sha256sum $L.vibd-new | cut -d' ' -f1)
	if [ "$got" != "$1" ]; then echo "launchd.plist upload corrupt, cache NOT replaced"; rm -f $L.vibd-new; exit 1; fi
	chown root:wheel $L.vibd-new; chmod 644 $L.vibd-new
	/var/jb/bin/mv -f $L.vibd-new $L && echo "job com.sapphire.vibd added to launchd's cache"
fi
N=/var/tmp/vibd-new
chown root:wheel $N/*; chmod 755 $N/vibd $N/SapphireHaptics.dylib; chmod 644 $N/*.plist
/var/jb/bin/mv -f $N/vibd /redwood/bin/vibd
/var/jb/bin/mv -f $N/com.sapphire.vibd.plist /redwood/share/vibd/
/var/jb/bin/mv -f $N/SapphireHaptics.dylib $N/SapphireHaptics.plist /var/jb/usr/lib/TweakInject/
rmdir $N 2>/dev/null
if launchctl print system/com.sapphire.vibd >/dev/null 2>&1; then
	launchctl kickstart -k system/com.sapphire.vibd && echo "vibd restarted"
elif launchctl bootstrap system /redwood/share/vibd/com.sapphire.vibd.plist 2>/dev/null || launchctl load /redwood/share/vibd/com.sapphire.vibd.plist 2>/dev/null; then
	echo "vibd loaded"
else
	nohup /redwood/bin/vibd >> /var/tmp/vibd.out 2>&1 & echo "vibd started by hand (launchctl refused)"
fi
sleep 2; pgrep -l vibd; tail -3 /var/tmp/vibd.log 2>/dev/null
if [ "$2" = 1 ]; then killall -9 SpringBoard; echo "SpringBoard restarted"; fi
EOF
