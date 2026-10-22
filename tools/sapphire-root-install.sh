#!/bin/bash
# Copy a directory into the iOS HFS+ root image as new files, as root in WSL:
#
#   sudo tools/sapphire-root-install.sh [--replace] IMG SRC DEST OUTDIR
#
#   e.g. sudo tools/sapphire-root-install.sh ~/ios-build/reset/pristine.img \
#            metal/out/SapphireMetal.bundle /System/Library/Extensions/SapphireMetal.bundle /tmp/metal
#
# IMG must carry the phone's current HFS+ metadata (NOTES: copy the metadata ranges from the
# phone first, with a clean journal). DEST must not exist yet, unless --replace, which deletes it
# first. Either way OUTDIR/files.runs lists the physical blocks of every file under DEST
# (FIBMAP, while mounted): a replaced file may land on blocks the old one freed, which no bitmap
# difference shows, so those runs go back to the phone too. Files are owned by
# root:wheel, 0755 for directories and executables, 0644 otherwise. OUTDIR gets the bitmap
# before and after; tools/hfs-push-ranges.py (or blocks.py) turns them and the metadata ranges
# into the blocks to write back.
set -euo pipefail
replace=0
[ "${1:-}" = --replace ] && { replace=1; shift; }
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
if [ -e "$mnt$dest" ]; then
	[ $replace = 1 ] || { echo "$dest already exists in the image (--replace to overwrite)" >&2; exit 1; }
	rm -rf "$mnt$dest"
	sync
fi
mkdir -p "$(dirname "$mnt$dest")"
cp -R "$src" "$mnt$dest"
chown -R 0:0 "$mnt$dest"
find "$mnt$dest" -type d -exec chmod 755 {} +
find "$mnt$dest" -type f -exec chmod 644 {} +
# executables keep their x bit (the bundle's binary is built with it)
find "$src" -type f -perm /111 -printf '%P\n' | while read -r f; do chmod 755 "$mnt$dest/$f"; done
ls -lRn "$mnt$dest"
sync
# the physical blocks of every installed file (4 KB filesystem blocks = image blocks)
python3 -I - "$mnt$dest" > "$out/files.runs" <<'PY'
import fcntl, os, struct, sys
FIBMAP = 1
runs = []
for root, dirs, files in os.walk(sys.argv[1]):
    for name in files:
        path = os.path.join(root, name)
        n = (os.path.getsize(path) + 4095) // 4096
        fd = os.open(path, os.O_RDONLY)
        for i in range(n):
            b = struct.unpack('I', fcntl.ioctl(fd, FIBMAP, struct.pack('I', i)))[0]
            if b == 0:
                sys.exit('%s: block %d not mapped' % (path, i))
            if runs and runs[-1][0] + runs[-1][1] == b:
                runs[-1][1] += 1
            else:
                runs.append([b, 1])
        os.close(fd)
for s, c in runs:
    print(s, c)
PY
echo "file blocks: $(wc -l < "$out/files.runs") runs, $(awk '{n+=$2} END {print n+0}' "$out/files.runs") blocks"
umount "$mnt"
trap - EXIT
dd if="$img" bs=4096 skip=1 count=128 of="$out/bitmap.after" status=none
cmp -s "$out/bitmap.before" "$out/bitmap.after" && echo "bitmap unchanged (?)" || echo "bitmap changed: new blocks allocated"
