#!/bin/bash
# Build the iOS root image for sapphire in one go (Linux or WSL2, as root):
#
#   sudo tools/sapphire-root.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUTDIR [hfsplus.ko]
#
# The dmgs are the D22AP Erase identity's OS, Cryptex1,SystemOS and
# Cryptex1,AppOS images from the iPhone10,3 16.7.16 (20H392) IPSW. WSL2's
# stock kernel has no HFS+: pass an hfsplus.ko built for the running kernel
# and it is loaded first. Output: OUTDIR/ios16-root.img and its sparse
# OUTDIR/ios16-root.simg for `fastboot flash userdata`.
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,11p' "$0"; exit 2; }
[[ $EUID == 0 ]] || { echo "run as root (sudo)" >&2; exit 1; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3") outdir=$(realpath "$4") ko=${5:-}
here=$(dirname "$(realpath "$0")")
img=$outdir/ios16-root.img

if ! grep -qw hfsplus /proc/filesystems; then
	[[ -n $ko ]] || { echo "no HFS+ in this kernel: pass hfsplus.ko" >&2; exit 1; }
	insmod "$ko"
fi

bash "$here/mkiosroot.sh" "$os" "$sysos" "$appos" "$img" 16G

mnt=$(mktemp -d)
fsck.hfsplus -fy "$img" | grep -v 'Illegal name' || true
mount -t hfsplus -o loop,force "$img" "$mnt"
trap 'mountpoint -q "$mnt" && umount "$mnt"; rmdir "$mnt"' EXIT
python3 "$here/sweet-root-tweaks.py" "$mnt"
umount "$mnt"
fsck.hfsplus -f -n "$img"

img2simg "$img" "$outdir/ios16-root.simg"
ls -la "$img" "$outdir/ios16-root.simg"
