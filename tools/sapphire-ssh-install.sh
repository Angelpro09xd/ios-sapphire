#!/bin/bash
# Install the Procursus rootless bootstrap and an sshd (key only) into the
# iOS HFS+ root image, as root in WSL:
#
#   sudo tools/sapphire-ssh-install.sh IMG BOOTSTRAP.tar.zst AUTHORIZED_KEYS OUTDIR
#
# IMG must carry the phone's current HFS+ metadata (see NOTES: copy the
# metadata ranges from the phone first). Only new files are written, and
# launchd.plist is replaced through a new file + rename, so every data block
# that changes is newly allocated in the volume bitmap. OUTDIR gets the
# bitmap before and after, from which the blocks to write back are derived.
set -euo pipefail
img=$1 bs=$2 keys=$3 out=$4
here=$(dirname "$(realpath "$0")")
mnt=/mnt/ios
mkdir -p "$out" "$mnt"
mountpoint -q "$mnt" && { echo "$mnt already mounted" >&2; exit 1; }
grep -qw hfsplus /proc/filesystems || insmod /home/angel/ios-build/hfsplus.ko

# bitmap before (allocation file is blocks 1..128 on this volume; checked)
dd if="$img" bs=4096 skip=1 count=128 of="$out/bitmap.before" status=none

stage=$(mktemp -d)
trap 'mountpoint -q "$mnt" && umount "$mnt"; rm -rf "$stage"' EXIT
zstd -dc "$bs" | tar -x -C "$stage" -f -
test -x "$stage/var/jb/usr/sbin/sshd"

# sshd: inetd from launchd, keys only, no PAM; host keys made here
cfg=$stage/var/jb/etc/ssh/sshd_config
sed -i 's/^UsePAM yes/UsePAM no/' "$cfg"
printf '\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nPubkeyAuthentication yes\n' >> "$cfg"
for t in rsa ecdsa ed25519; do
	ssh-keygen -q -t $t -N "" -C "" -f "$stage/var/jb/etc/ssh/ssh_host_${t}_key"
done
rm -f "$stage/var/jb/prep_bootstrap.sh"

mount -t hfsplus -o loop,force "$img" "$mnt"
test -d "$mnt/private/var" && test ! -e "$mnt/private/var/jb"
cp -a "$stage/var/jb" "$mnt/private/var/jb"

# root's login shell is /bin/sh, which iOS does not ship
mkdir -p "$mnt/bin"
[ -e "$mnt/bin/sh" ] || ln -s /var/jb/bin/sh "$mnt/bin/sh"
[ -e "$mnt/bin/bash" ] || ln -s /var/jb/bin/bash "$mnt/bin/bash"

install -d -m 700 -o 0 -g 0 "$mnt/private/var/root/.ssh"
install -m 600 -o 0 -g 0 "$keys" "$mnt/private/var/root/.ssh/authorized_keys"

# sshd job in the launchd cache (new file, then rename over the old one)
lp=$mnt/System/Library/xpc/launchd.plist
python3 -I - "$lp" "$stage/var/jb/Library/LaunchDaemons/com.openssh.sshd.plist" <<'EOF'
import plistlib, sys
lp, job = sys.argv[1], sys.argv[2]
p = plistlib.load(open(lp, 'rb'))
j = plistlib.load(open(job, 'rb'))
j['StandardErrorPath'] = '/var/jb/var/log/sshd.log'
p['LaunchDaemons']['/var/jb/Library/LaunchDaemons/com.openssh.sshd.plist'] = j
with open(lp + '.new', 'wb') as f:
    plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
print('launchd.plist:', len(p['LaunchDaemons']), 'jobs')
EOF
mkdir -p "$mnt/private/var/jb/var/log"
chown --reference="$lp" "$lp.new"
chmod --reference="$lp" "$lp.new"
getfattr -d -m - --absolute-names "$lp" > "$out/launchd.plist.xattrs" || true
mv "$lp.new" "$lp"
[ -s "$out/launchd.plist.xattrs" ] && setfattr --restore="$out/launchd.plist.xattrs"
getfattr -d -m - --absolute-names "$lp" | diff - "$out/launchd.plist.xattrs" && echo "launchd.plist xattrs kept"
sync
umount "$mnt"

dd if="$img" bs=4096 skip=1 count=128 of="$out/bitmap.after" status=none
echo done
