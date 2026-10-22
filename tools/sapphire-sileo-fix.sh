#!/bin/bash
# Sileo's settings and apt's, in the iOS root image, as root in WSL (after mA.sh, before mC.sh):
#   - ShowProvisional (Canister search) off: its servers cannot be reached, the asking never stops and
#     the package downloads never get their turn (HackiOS's finding; their inject_app.c sets it at start).
#     Written in the global preferences and in Sileo's data container, owned by mobile.
#   - apt stays root (APT::Sandbox::User): the prefix has no pwd.db, so "_apt" cannot be looked up.
#   - Sileo's dead downloads (0-byte CFNetworkDownload_*.tmp) removed.
# OUTDIR/files.runs: the blocks of the files written.
#   sapphire-sileo-fix.sh IMG OUTDIR
set -euo pipefail
img=$1 out=$2
mnt=/mnt/ios
mkdir -p "$out" "$mnt"
grep -qw hfsplus /proc/filesystems || insmod /home/angel/ios-build/hfsplus.ko
trap 'mountpoint -q "$mnt" && umount "$mnt"' EXIT
mount -t hfsplus -o loop,force "$img" "$mnt"
files=""
write_prefs() {
	python3 -I -c 'import plistlib, sys; p = sys.argv[1]
d = {}
try:
    d = plistlib.load(open(p, "rb"))
except Exception:
    pass
d["ShowProvisional"] = False
plistlib.dump(d, open(p, "wb"), fmt=plistlib.FMT_BINARY)' "$1"
	chown 501:501 "$1"
	chmod 600 "$1"
	files="$files $1"
}
write_prefs "$mnt/private/var/mobile/Library/Preferences/org.coolstar.SileoStore.plist"
for c in "$mnt"/private/var/mobile/Containers/Data/Application/*/; do
	if [ -d "$c/Library/Caches/org.coolstar.SileoStore" ]; then
		mkdir -p "$c/Library/Preferences"
		write_prefs "$c/Library/Preferences/org.coolstar.SileoStore.plist"
		find "$c/tmp" -maxdepth 1 -name 'CFNetworkDownload_*.tmp' -size 0 -delete
		echo "Sileo's container: ${c#$mnt}"
	fi
done
conf="$mnt/private/var/jb/etc/apt/apt.conf.d/20sapphire-sandbox.conf"
echo 'APT::Sandbox::User "root";' > "$conf"
chmod 644 "$conf"
files="$files $conf"
sync
python3 -I - $files > "$out/files.runs" <<'PY'
import fcntl, os, struct, sys
runs = []
for p in sys.argv[1:]:
    n = (os.path.getsize(p) + 4095) // 4096
    fd = os.open(p, os.O_RDONLY)
    for i in range(n):
        b = struct.unpack('I', fcntl.ioctl(fd, 1, struct.pack('I', i)))[0]
        if b:
            runs.append((b, 1))
    os.close(fd)
for s, c in runs:
    print(s, c)
PY
echo "written:$files"
