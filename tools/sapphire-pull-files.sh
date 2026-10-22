#!/bin/bash
# Pull files from the phone's iOS root (HFS+ on sda18) without copying the partition, in WSL,
# phone in recovery, after mA.sh has put the phone's current metadata into IMG:
#   sapphire-pull-files.sh IMG DIR_ON_IOS OUTDIR [NEWEST_N]
# The image is mounted read-only (root, for FIBMAP) to learn each file's blocks; the blocks
# are then read off the phone with adb. Default: the 12 newest files under DIR_ON_IOS.
set -euo pipefail
img=$1 dir=$2 out=$3 n=${4:-12}
A="/mnt/c/adb/adb.exe -s fcd8b95d"
mkdir -p "$out"
runs=$out/runs.txt
echo "${SUDO_PASS:?SUDO_PASS (the WSL sudo password) in the environment}" | sudo -S python3 -I - "$img" "$dir" "$n" > "$runs" <<'PY'
import fcntl, os, struct, subprocess, sys
img, d, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
m = '/mnt/ios-ro'
os.makedirs(m, exist_ok=True)
subprocess.check_call(['mount', '-t', 'hfsplus', '-o', 'loop,ro', img, m])
try:
    base = m + d
    files = []
    for root, _, names in os.walk(base):
        for x in names:
            p = os.path.join(root, x)
            if os.path.isfile(p) and not os.path.islink(p):
                files.append((os.path.getmtime(p), p))
    for t, p in sorted(files)[-n:]:
        size = os.path.getsize(p)
        fd = os.open(p, os.O_RDONLY)
        blocks = [struct.unpack('I', fcntl.ioctl(fd, 1, struct.pack('I', i)))[0] for i in range((size + 4095) // 4096)]
        os.close(fd)
        print('%d\t%s\t%s' % (size, p[len(m):], ' '.join(map(str, blocks))))
finally:
    subprocess.call(['umount', m])
PY
while IFS=$'\t' read -r size path blocks; do
	name=$(echo "$path" | tr '/' '_')
	: > "$out/$name"
	for b in $blocks; do
		$A exec-out "dd if=/dev/block/sda18 bs=4096 skip=$b count=1 2>/dev/null" < /dev/null >> "$out/$name"
	done
	truncate -s "$size" "$out/$name"
	echo "$size $path"
done < "$runs"
