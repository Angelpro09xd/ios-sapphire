#!/bin/bash
# SSH into the phone over the USB net (192.168.7.2), in the iOS root image, as root in WSL
# (after mA.sh, before mC.sh):
#   - host keys made here (KEYDIR, kept out of git) into /var/jb/etc/ssh, 0600: the prefix has no
#     pwd.db, so ssh-keygen in sshd-keygen-wrapper cannot look up root and makes none
#   - PUBKEY added to the authorized_keys of root (/var/root and /var/jb/var/root) and mobile
#   - /bin/sh and /bin/bash as real copies (links made from Linux are not followed on the phone)
#   - pwd.db, spwd.db and passwd made here from the image's /var/jb/etc/master.passwd with MKPWDB
#     (tools/mkpwdb.c built for Linux): libiosexec looks users up only in the dbs, so without them
#     sshd and PAM know nobody. It reads OpenBSD's record format, which FreeBSD's pwd_mkdb no longer
#     writes. launchd does not run RunAtLoad jobs from /var/jb, so they cannot be made at boot;
#     run this again after anything adds users to master.passwd
# OUTDIR/files.runs: the blocks of the files written.
#   sapphire-ssh-setup.sh IMG KEYDIR PUBKEY MKPWDB OUTDIR
set -euo pipefail
img=$1 keys=$2 pub=$3 mkdb=$4 out=$5
mnt=/mnt/ios
mkdir -p "$out" "$mnt" "$keys"
for t in rsa ecdsa ed25519; do
	[ -f "$keys/ssh_host_${t}_key" ] || ssh-keygen -q -t $t -N "" -C "" -f "$keys/ssh_host_${t}_key"
done
grep -qw hfsplus /proc/filesystems || insmod /home/angel/ios-build/hfsplus.ko
trap 'mountpoint -q "$mnt" && umount "$mnt"' EXIT
mount -t hfsplus -o loop,force "$img" "$mnt"
jb=$mnt/private/var/jb
files=""
put() { # put SRC DEST MODE OWNER
	rm -f "$2"
	cat "$1" > "$2"
	chmod "$3" "$2"
	chown "$4" "$2"
	files="$files $2"
}
for t in rsa ecdsa ed25519; do
	put "$keys/ssh_host_${t}_key" "$jb/etc/ssh/ssh_host_${t}_key" 600 0:0
	put "$keys/ssh_host_${t}_key.pub" "$jb/etc/ssh/ssh_host_${t}_key.pub" 644 0:0
done
auth() { # auth HOME OWNER
	mkdir -p "$1/.ssh"
	chown "$2" "$1/.ssh"
	chmod 700 "$1/.ssh"
	local a=$1/.ssh/authorized_keys tmp
	tmp=$(mktemp)
	{ [ -f "$a" ] && grep -vxF "$(cat "$pub")" "$a" || true; cat "$pub"; } > "$tmp"
	put "$tmp" "$a" 600 "$2"
	rm -f "$tmp"
}
auth "$mnt/private/var/root" 0:0
auth "$jb/var/root" 0:0
auth "$jb/var/mobile" 501:501
for b in sh bash; do
	src=$(readlink -f "$jb/bin/$b")
	test -f "$src"
	put "$src" "$mnt/bin/$b" 755 0:0
done
db=$(mktemp -d)
"$mkdb" "$jb/etc/master.passwd" "$db"
grep -v '^#' "$jb/etc/master.passwd" | awk -F: 'NF == 10 { print $1 ":*:" $3 ":" $4 ":" $8 ":" $9 ":" $10 }' > "$db/passwd"
put "$db/pwd.db" "$jb/etc/pwd.db" 644 0:0
put "$db/spwd.db" "$jb/etc/spwd.db" 600 0:0
put "$db/passwd" "$jb/etc/passwd" 644 0:0
rm -rf "$db"
rm -f "$jb/Library/LaunchDaemons/dev.sapphire.pwdmkdb.plist"
sync
python3 -I - $files > "$out/files.runs" <<'PY'
import fcntl, os, struct, sys
for p in sys.argv[1:]:
    n = (os.path.getsize(p) + 4095) // 4096
    fd = os.open(p, os.O_RDONLY)
    for i in range(n):
        b = struct.unpack('I', fcntl.ioctl(fd, 1, struct.pack('I', i)))[0]
        if b:
            print(b, 1)
    os.close(fd)
PY
ls -ln $files
echo "written: $(echo $files | wc -w) files, $(wc -l < "$out/files.runs") blocks"
