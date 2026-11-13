set -u
H=/home/angel/ios-sapphire/installer/tools-arm64/heic
M=/tmp/heic-os
mkdir -p $M
mountpoint -q $M || apfs-fuse /home/angel/ipsw/087-86698-044.dmg $M || exit 1
C="$M/root/Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection"
WP="$C/Wallpapers/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper"
N=$H/wallpaper-notes; mkdir -p $N $H/testdata $N/raw
echo "=== collection top"; ls -la "$C"; ls -la "$C/Wallpapers"
echo "=== listing"
( cd "$WP" && find . -printf '%y %10s %p\n' | sort -k3 ) | tee $N/listing-375w-812h@3x.txt
# copy the whole wallpaper folder (raw, for later recipe work)
rm -rf "$N/raw/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper"
cp -a "$WP" "$N/raw/"
# copy HEICs preserving relative paths
( cd "$WP" && find . -iname '*.heic' -print0 | cpio -0pdm --quiet $H/testdata )
# collection-level plists + other files
mkdir -p $N/raw/collection
find "$C" -maxdepth 1 -type f -exec cp -a {} $N/raw/collection/ \;
cp -a "$M/root/Library/Wallpaper/DefaultWallpapers~iphone.plist" "$M/root/Library/Wallpaper/Collections/Collections~iphone.plist" $N/raw/collection/
find "$C" -maxdepth 2 -name '*.plist' | head -50
# grep for references to 7400 in plists in Wallpaper dir
grep -rl --binary-files=text 'WWDC_2022' "$M/root/Library/Wallpaper" 2>/dev/null | grep -v '\.ca/' | head
# other sizes for comparison
for d in "$C/Wallpapers"/7400*; do echo "== $d"; ls -la "$d"; done
fusermount3 -u $M || fusermount -u $M
mountpoint -q $M && echo STILL MOUNTED || echo unmounted
