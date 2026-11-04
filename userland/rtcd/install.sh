#!/bin/bash
# install.sh [PHONE]: rtcd onto the phone: /redwood/bin/rtcd, the job com.sapphire.rtcd in the launchd cache
# (../haptics/launchd-job.py; every replaced cache kept in ~/ios-build/launchd-backups/), started now.
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=8 -i $HOME/.ssh/sapphire_phone"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
/usr/lib/llvm-18/bin/clang -target arm64-apple-ios15.0 -isysroot "$SDK" -fuse-ld=lld -O2 -Wall -o "$out/rtcd" rtcd.c
"$HOME/tools-dl/ldid/ldid" -Sent.xml "$out/rtcd"
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-rtcd.$stamp"
python3 -I ../haptics/launchd-job.py add com.sapphire.rtcd.plist "$bk/launchd.plist.before-rtcd.$stamp" "$out/launchd.plist"
scp -q $O "$out/rtcd" "$out/launchd.plist" com.sapphire.rtcd.plist $P:/var/tmp/
sum=$(sha256sum "$out/launchd.plist" | cut -d" " -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" <<'PH'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
L=/System/Library/xpc/launchd.plist
[ "$(sha256sum /var/tmp/launchd.plist | cut -d' ' -f1)" = "$1" ] || { echo "cache upload corrupt"; exit 1; }
chown root:wheel /var/tmp/launchd.plist /var/tmp/rtcd /var/tmp/com.sapphire.rtcd.plist
chmod 644 /var/tmp/launchd.plist /var/tmp/com.sapphire.rtcd.plist
chmod 755 /var/tmp/rtcd
mkdir -p /redwood/share/rtcd
mv -f /var/tmp/rtcd /redwood/bin/rtcd
mv -f /var/tmp/com.sapphire.rtcd.plist /redwood/share/rtcd/
mv -f /var/tmp/launchd.plist $L && echo "job com.sapphire.rtcd in the launchd cache"
/redwood/bin/rtcd --once
launchctl bootstrap system /redwood/share/rtcd/com.sapphire.rtcd.plist 2>/dev/null || launchctl kickstart -k system/com.sapphire.rtcd 2>/dev/null
sleep 1
launchctl print system/com.sapphire.rtcd 2>/dev/null | grep -E "state|pid" | head -2
PH
