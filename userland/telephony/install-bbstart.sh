#!/bin/bash
# install-bbstart.sh [PHONE]: locationd and abm-helper started through /redwood/bin/bbstart (the fake
# AppleBaseband, fakebb.c): both files onto the phone, the two jobs in launchd's cache pointed at
# bbstart (the cache before kept in ~/ios-build/launchd-backups/). Takes effect at the next boot.
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=8 -i $HOME/.ssh/sapphire_phone"
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
"$LDID" -S ../obj/bbstart
"$LDID" -S ../obj/libsapphire-fakebb.dylib
bk=$HOME/ios-build/launchd-backups
mkdir -p "$bk"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/System/Library/xpc/launchd.plist "$bk/launchd.plist.before-bbstart.$stamp"
python3 -I - "$bk/launchd.plist.before-bbstart.$stamp" /tmp/launchd.plist.bbstart <<"PY"
import plistlib, sys
src, dst = sys.argv[1:]
raw = open(src, "rb").read()
fmt = plistlib.FMT_BINARY if raw[:8] == b"bplist00" else plistlib.FMT_XML
p = plistlib.loads(raw)
d = p["LaunchDaemons"]
real = {"com.apple.locationd": "/usr/libexec/locationd",
        "com.apple.abm-helper": "/System/Library/PrivateFrameworks/ABMHelper.framework/Support/abm-helper"}
for label, program in real.items():
    job = d["/System/Library/LaunchDaemons/%s.plist" % label]
    args = job.get("ProgramArguments") or [job.get("Program", program)]
    args = [a for a in args if a not in ("/redwood/bin/locstart", "/redwood/bin/bbstart")]
    if not args or args[0] != program:
        args = [program] + args[1:]
    job["ProgramArguments"] = ["/redwood/bin/bbstart"] + args
    job.pop("Program", None)
    print(label, "->", job["ProgramArguments"])
plistlib.dump(p, open(dst, "wb"), fmt=fmt)
PY
scp -q $O ../obj/bbstart ../obj/libsapphire-fakebb.dylib /tmp/launchd.plist.bbstart $P:/var/tmp/
sum=$(sha256sum /tmp/launchd.plist.bbstart | cut -d" " -f1)
ssh $O $P /var/jb/bin/bash -s "$sum" <<"PH"
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
[ "$(sha256sum /var/tmp/launchd.plist.bbstart | cut -d" " -f1)" = "$1" ] || { echo "cache upload corrupt"; exit 1; }
chown root:wheel /var/tmp/bbstart /var/tmp/libsapphire-fakebb.dylib /var/tmp/launchd.plist.bbstart
chmod 755 /var/tmp/bbstart /var/tmp/libsapphire-fakebb.dylib
chmod 644 /var/tmp/launchd.plist.bbstart
mv -f /var/tmp/bbstart /redwood/bin/bbstart
mv -f /var/tmp/libsapphire-fakebb.dylib /redwood/lib/libsapphire-fakebb.dylib
mv -f /var/tmp/launchd.plist.bbstart /System/Library/xpc/launchd.plist && echo "launchd cache: locationd, abm-helper via bbstart"
PH
