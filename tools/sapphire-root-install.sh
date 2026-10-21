#!/bin/bash
# Copy a directory into the iOS HFS+ root image as new files, as root in WSL:
#
#   sudo tools/sapphire-root-install.sh IMG SRC DEST OUTDIR
#
#   e.g. sudo tools/sapphire-root-install.sh ~/ios-build/reset/pristine.img \
#            metal/out/SapphireMetal.bundle /System/Library/Extensions/SapphireMetal.bundle /tmp/metal
#
# IMG must carry the phone's current HFS+ metadata (NOTES: copy the metadata ranges from the
# phone first, with a clean journal). DEST must not exist yet: only new files are written, so
# every data block that changes is newly allocated in the volume bitmap. Files are owned by
# root:wheel, 0755 for directories and executables, 0644 otherwise. OUTDIR gets the bitmap
# before and after; tools/hfs-push-ranges.py (or blocks.py) turns them and the metadata ranges
# into the blocks to write back.
set -euo pipefail
img=$1 src=$2 dest=$3 out=$4
mnt=/mnt/ios
mkdir -p "$out" "$mnt"
mountpoint -q "$mnt" && { echo "$mnt already mounted" >&2; exit 1; }
grep -qw hfsplus /proc/filesystems || insmod /home/angel/ios-build/hfsplus.ko
test -d "$src"

# bitmap before (allocation file is blocks 1..128 on this volume; checked)
dd if="$img" bs=4096 skip=1 count=128 of="$out/bitmap.before" status=none
trap 'mountpoint -q "$mnt" && umount "$mnt"' EXIT
mount -t hfsplus -o loop,force "$img" "$mnt"
test -d "$mnt/System/Library"
[ -e "$mnt$dest" ] && { echo "$dest already exists in the image" >&2; exit 1; }
mkdir -p "$(dirname "$mnt$dest")"
cp -R "$src" "$mnt$dest"
chown -R 0:0 "$mnt$dest"
find "$mnt$dest" -type d -exec chmod 755 {} +
find "$mnt$dest" -type f -exec chmod 644 {} +
# executables keep their x bit (the bundle's binary is built with it)
find "$src" -type f -perm /111 -printf '%P\n' | while read -r f; do chmod 755 "$mnt$dest/$f"; done
ls -lRn "$mnt$dest"
sync
umount "$mnt"
trap - EXIT
dd if="$img" bs=4096 skip=1 count=128 of="$out/bitmap.after" status=none
cmp -s "$out/bitmap.before" "$out/bitmap.after" && echo "bitmap unchanged (?)" || echo "bitmap changed: new blocks allocated"
