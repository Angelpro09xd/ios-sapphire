#!/bin/bash
# install.sh [PHONE]: camd onto the phone (root@192.168.7.2, key ~/.ssh/sapphire_phone), from WSL; rerun = update.
#   /redwood/bin/camd, /redwood/bin/camctl (+ /redwood/share/camd/com.sapphire.camd.plist), the job com.sapphire.camd in
#   launchd's cache (../../haptics/launchd-job.py add; the cache before the first install kept on the phone as
#   launchd.plist.pre-camd, every replaced cache also in ~/ios-build/launchd-backups/ here).
# camd is (re)started now (kickstart, or bootstrap of the plist). The previous binaries stay as *.prev
# (uninstall.sh --rollback puts them back).
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
JOB=../../haptics/launchd-job.py
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
bash build.sh "$out" > /dev/null
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
mkdir -p /redwood/bin /redwood/share/camd /var/tmp/camd-new
L=/System/Library/xpc/launchd.plist
[ -f $L.pre-camd ] || /var/jb/bin/cp -p $L $L.pre-camd
for f in /redwood/bin/camd /redwood/bin/camctl; do [ -f $f ] && /var/jb/bin/cp -p $f $f.prev; done
true
EOF
scp -q $O "$out/camd" "$out/camctl" "$out/com.sapphire.camd.plist" $P:/var/tmp/camd-new/

bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-camd-install.$stamp"
newcache=0
if ! python3 -I -c "import plistlib,sys;sys.exit(0 if '/System/Library/LaunchDaemons/com.sapphire.camd.plist' in plistlib.load(open(sys.argv[1],'rb'))['LaunchDaemons'] else 1)" \
	"$bk/launchd.plist.before-camd-install.$stamp"; then
	python3 -I $JOB add com.sapphire.camd.plist "$bk/launchd.plist.before-camd-install.$stamp" "$out/launchd.plist"
	scp -q $O "$out/launchd.plist" $P:/System/Library/xpc/launchd.plist.camd-new
	newcache=1
fi
sum=$( [ $newcache = 1 ] && sha256sum "$out/launchd.plist" | cut -d' ' -f1 || echo none)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
if [ "$1" != none ]; then
	got=$(shasum -a 256 $L.camd-new 2>/dev/null | cut -d' ' -f1)
	[ -z "$got" ] && got=$(sha256sum $L.camd-new | cut -d' ' -f1)
	if [ "$got" != "$1" ]; then echo "launchd.plist upload corrupt, cache NOT replaced"; rm -f $L.camd-new; exit 1; fi
	chown root:wheel $L.camd-new; chmod 644 $L.camd-new
	/var/jb/bin/mv -f $L.camd-new $L && echo "job com.sapphire.camd added to launchd's cache"
fi
N=/var/tmp/camd-new
chown root:wheel $N/*; chmod 755 $N/camd $N/camctl; chmod 644 $N/*.plist
/var/jb/bin/mv -f $N/camd $N/camctl /redwood/bin/
/var/jb/bin/mv -f $N/com.sapphire.camd.plist /redwood/share/camd/
rmdir $N 2>/dev/null
if launchctl print system/com.sapphire.camd >/dev/null 2>&1; then
	launchctl kickstart -k system/com.sapphire.camd && echo "camd restarted"
elif launchctl bootstrap system /redwood/share/camd/com.sapphire.camd.plist 2>/dev/null || launchctl load /redwood/share/camd/com.sapphire.camd.plist 2>/dev/null; then
	echo "camd loaded"
else
	nohup /redwood/bin/camd --no-wait >> /var/tmp/camd.out 2>&1 & echo "camd started by hand (launchctl refused)"
fi
sleep 2; pgrep -l camd; tail -5 /var/tmp/camd.log 2>/dev/null
EOF
