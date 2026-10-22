#!/bin/bash
# The package manager's prefix (userland/jb/mk-jb.sh: Procursus rootless + Sileo + ElleKit) into the
# iOS root image, as root in WSL, after mA.sh and before mC.sh (as HackiOS's build.sh and init.c set it up):
#   - TARBALL (made with owner 0:0, modes kept) unpacked to /private/var/jb, set-ID bits intact
#   - mobile's corner of it (Sileo's data and indexes) owned by 501
#   - each app of the prefix copied into /Applications (LaunchServices registers apps from there;
#     not from /private/var, and not a bundle of links)
#   - /bin/{chmod,cp,mkdir,mv,rm} -> the prefix's own (Sileo runs them as root from /bin)
#   - the LaunchServices database removed, so the next boot registers the new apps
# OUTDIR/files.runs: the blocks of every file written.
#   sapphire-jb-install.sh IMG TARBALL OUTDIR
set -euo pipefail
img=$1 tarball=$2 out=$3
mnt=/mnt/ios
mkdir -p "$out" "$mnt"
mountpoint -q "$mnt" && { echo "$mnt already mounted" >&2; exit 1; }
grep -qw hfsplus /proc/filesystems || insmod /home/angel/ios-build/hfsplus.ko
trap 'mountpoint -q "$mnt" && umount "$mnt"' EXIT
mount -t hfsplus -o loop,force "$img" "$mnt"
test -d "$mnt/private/var"
rm -rf "$mnt/private/var/jb"
tar -C "$mnt/private/var" -xpzf "$tarball" --same-owner --numeric-owner
for d in var/mobile var/mobile/Library var/mobile/Library/Caches var/mobile/Library/Preferences var/mobile/Documents \
    var/lib/apt/sileolists var/lib/apt/sileolists/partial; do
	mkdir -p "$mnt/private/var/jb/$d"
done
chown -R 501:501 "$mnt/private/var/jb/var/mobile"
chown 501:501 "$mnt/private/var/jb/var/lib/apt/sileolists" "$mnt/private/var/jb/var/lib/apt/sileolists/partial"
apps=""
for app in "$mnt"/private/var/jb/Applications/*.app; do
	[ -d "$app" ] || continue
	n=$(basename "$app")
	rm -rf "$mnt/Applications/$n"
	(cd "$(dirname "$app")" && tar -cf - "$n") | (cd "$mnt/Applications" && tar -xpf - --same-owner)   # (cp copies ACLs: this hfsplus has none)
	apps="$apps /Applications/$n"
done
for c in chmod cp mkdir mv rm; do
	[ -e "$mnt/bin/$c" ] && [ ! -L "$mnt/bin/$c" ] && continue          # (iOS's own, if it has one)
	rm -f "$mnt/bin/$c"
	cp -p "$mnt/private/var/jb/usr/bin/$c" "$mnt/bin/$c"       # (a copy: a link made by Linux's hfsplus is not followed by dyld/XNU here)
	apps="$apps /bin/$c"
done
rm -f "$mnt"/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices-*.csstore
ls -l "$mnt"/private/var/jb/Applications/Sileo.app/giveMeRoot "$mnt/bin/mv"
echo "prefix: $(du -sh "$mnt/private/var/jb" | cut -f1); apps:$apps"
sync
python3 -I - "$mnt" /private/var/jb $apps > "$out/files.runs" <<'PY'
import fcntl, os, struct, sys
root = sys.argv[1]
runs = []
for top in sys.argv[2:]:
    for d, _, files in os.walk(root + top):
        for name in files:
            p = os.path.join(d, name)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            n = (os.path.getsize(p) + 4095) // 4096
            fd = os.open(p, os.O_RDONLY)
            for i in range(n):
                b = struct.unpack('I', fcntl.ioctl(fd, 1, struct.pack('I', i)))[0]
                if b == 0:
                    continue
                if runs and runs[-1][0] + runs[-1][1] == b:
                    runs[-1][1] += 1
                else:
                    runs.append([b, 1])
            os.close(fd)
for s, c in runs:
    print(s, c)
PY
echo "file blocks: $(wc -l < "$out/files.runs") runs, $(awk '{n+=$2} END {print n}' "$out/files.runs") blocks"
