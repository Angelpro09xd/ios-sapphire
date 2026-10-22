#!/bin/bash
# As root in WSL, after mA.sh (phone metadata in IMG, journal replayed), before mC.sh:
#   1. /redwood (Mesa + the EAGL bridge) replaced from SRC_REDWOOD
#   2. the apps iOS stages for its first boot (/private/var/staged_system_apps: Maps, Notes,
#      Weather, Clock, Calendar, ...) moved into /Applications, where they are system apps like
#      the rest (as HackiOS's select-system.py does): a rename on the same volume, no data moves.
# OUTDIR/files.runs: the blocks of the files written (for mC.sh).
#   sapphire-apps-gl.sh IMG SRC_REDWOOD OUTDIR
set -euo pipefail
img=$1 src=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
bash "$here/sapphire-root-install.sh" --replace "$img" "$src" /redwood "$out"
mnt=/mnt/ios
mkdir -p "$mnt"
trap 'mountpoint -q "$mnt" && umount "$mnt"' EXIT
mount -t hfsplus -o loop,force "$img" "$mnt"
s="$mnt/private/var/staged_system_apps"
moved=0
if [ -d "$s" ]; then
	for a in "$s"/*.app; do
		[ -e "$a" ] || continue
		n=$(basename "$a")
		if [ -e "$mnt/Applications/$n" ]; then
			echo "already in /Applications: $n"
			continue
		fi
		mv "$a" "$mnt/Applications/$n"
		moved=$((moved + 1))
	done
fi
sync
echo "moved $moved apps into /Applications; /Applications now has $(ls "$mnt/Applications" | wc -l) entries"
umount "$mnt"
