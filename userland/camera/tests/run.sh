#!/bin/sh
# Round-trip tests for the camera pipeline (host build).
#   sh tests/run.sh [photo.png|photo.ppm ...]
# For each test image: simulate the sensor (rawsynth), process it back (rawtool), compare
# with the reference (PSNR in dB, 16-bit sRGB, 8-pixel border ignored). Photos given as
# arguments are used at their own resolution (sharp content is the hard case).
# Work files go to $OUT (default /tmp/camtest-$USER).
set -e
cd "$(dirname "$0")/.."
B=build/host
OUT=${OUT:-/tmp/camtest-$(id -un)}
mkdir -p "$OUT"
[ -x $B/rawtool ] && [ -x $B/rawsynth ] || make >/dev/null
GAINS="--wb 1.818182,1,1.428571" # = 1 / rawsynth's default --sens 0.55,1,0.7
fail=0

psnr() { $B/rawtool psnr "$1" "$2" | sed 's/.*all //; s/ dB//'; }
check() { # name value min
	if awk "BEGIN{exit !($2 >= $3)}"; then :; else echo "FAIL: $1 = $2 dB < $3"; fail=1; fi
}

row() { # name size rawsynth-source-args...
	name=$1 size=$2
	shift 2
	$B/rawsynth sensor "$@" --size "$size" --cfa grbg --cell 3 --align 32 -o "$OUT/n.raw" --ref "$OUT/ref.ppm" \
		--ref-bayer "$OUT/refb.pgm" --ref-cfa rggb 2>/dev/null
	$B/rawsynth sensor "$@" --size "$size" --cfa grbg --cell 1 -o "$OUT/b.raw" 2>/dev/null
	P="--size $size --cfa grbg $GAINS --bits 16"
	$B/rawtool $P --cell 3 --remosaic simple --ppm "$OUT/s.ppm" "$OUT/n.raw" 2>/dev/null
	$B/rawtool $P --cell 3 --ppm "$OUT/d.ppm" --bayer "$OUT/bay.pgm" "$OUT/n.raw" 2>/dev/null
	$B/rawtool $P --cell 3 --via-bayer --ppm "$OUT/v.ppm" "$OUT/n.raw" 2>/dev/null
	$B/rawtool $P --cell 1 --ppm "$OUT/m.ppm" "$OUT/b.raw" 2>/dev/null
	$B/rawtool $P --cell 1 --demosaic bilinear --ppm "$OUT/l.ppm" "$OUT/b.raw" 2>/dev/null
	s=$(psnr "$OUT/ref.ppm" "$OUT/s.ppm") d=$(psnr "$OUT/ref.ppm" "$OUT/d.ppm") v=$(psnr "$OUT/ref.ppm" "$OUT/v.ppm")
	q=$(psnr "$OUT/refb.pgm" "$OUT/bay.pgm") m=$(psnr "$OUT/ref.ppm" "$OUT/m.ppm") l=$(psnr "$OUT/ref.ppm" "$OUT/l.ppm")
	printf "%-14s %7s %7s %7s %7s | %7s %7s\n" "$name" "$s" "$d" "$v" "$q" "$m" "$l"
	last_direct=$d last_simple=$s
}

echo "PSNR (dB) vs the reference image"
echo "               ---------- nonacell sensor ----------   -- Bayer sensor --"
printf "%-14s %7s %7s %7s %7s | %7s %7s\n" image simple guided via-Bay Bayer MHC bilin
for k in natural star bars gradient text zone; do
	row "$k" 1200x900 --kind $k
	case $k in
	natural) check "natural guided" "$last_direct" 38 ;;
	gradient) check "gradient guided" "$last_direct" 45 ;;
	star) check "star guided" "$last_direct" 36 ;;
	esac
done
for p in "$@"; do
	[ -f "$p" ] || continue
	sz=$($B/rawsynth sensor --in "$p" --cell 1 -o "$OUT/probe.raw" 2>&1 | sed -n 's/.*: \([0-9]*x[0-9]*\) .*/\1/p')
	w=${sz%x*} h=${sz#*x}
	sz=$((w / 6 * 6))x$((h / 6 * 6))
	row "$(basename "$p" | cut -c1-14)" "$sz" --in "$p" --crop 0,0,${sz%x*},${sz#*x}
done
echo "(simple = per-plane inverse-distance remosaic baseline; guided = default; via-Bay = guided remosaic"
echo " to RGGB then MHC demosaic; Bayer = remosaiced mosaic vs ideal RGGB mosaic; MHC/bilin = a real"
echo " Bayer sensor of the same size, for reference)"

echo
echo "functional checks"
R="$OUT/n.raw"
$B/rawsynth sensor --kind natural --size 1200x900 --cfa grbg --cell 3 --phase 4,1 --align 64 -o "$R" --ref "$OUT/ref.ppm" 2>/dev/null
$B/rawtool --size 1200x900 --cfa grbg --cell 3 --phase 4,1 $GAINS --bits 16 --ppm "$OUT/d.ppm" "$R" 2>/dev/null
v=$(psnr "$OUT/ref.ppm" "$OUT/d.ppm"); echo "phase 4,1 + 64-byte stride: $v dB"; check phase "$v" 36
$B/rawtool detect --size 1200x900 "$R" | grep -q "suggest: --cell 3 --phase 1,1 --cfa bggr or rggb" && echo "detect: ok" || { echo "FAIL: detect"; fail=1; }
$B/rawsynth sensor --kind natural --size 1200x900 --cfa rggb --cell 2 -o "$R" --ref "$OUT/ref.ppm" 2>/dev/null
$B/rawtool --size 1200x900 --cfa rggb --cell 2 $GAINS --bits 16 --ppm "$OUT/d.ppm" "$R" 2>/dev/null
v=$(psnr "$OUT/ref.ppm" "$OUT/d.ppm"); echo "tetracell: $v dB"; check tetracell "$v" 36
$B/rawsynth sensor --kind natural --size 1200x900 --cfa grbg --cell 3 --fmt raw16 -o "$R" --ref "$OUT/ref.ppm" 2>/dev/null
$B/rawtool --size 1200x900 --fmt raw16 --cfa grbg --cell 3 $GAINS --bits 16 --ppm "$OUT/d.ppm" "$R" 2>/dev/null
v=$(psnr "$OUT/ref.ppm" "$OUT/d.ppm"); echo "raw16 input: $v dB"; check raw16 "$v" 38
$B/rawsynth sensor --kind natural --size 1200x900 --cfa grbg --cell 3 --noise 2 --shot 0.5 -o "$R" --ref "$OUT/ref.ppm" 2>/dev/null
$B/rawtool --size 1200x900 --cfa grbg --cell 3 $GAINS --bits 16 --ppm "$OUT/d.ppm" "$R" 2>/dev/null
a=$(psnr "$OUT/ref.ppm" "$OUT/d.ppm")
$B/rawtool --size 1200x900 --cfa grbg --cell 3 $GAINS --bits 16 --denoise 1 --ppm "$OUT/d.ppm" "$R" 2>/dev/null
b=$(psnr "$OUT/ref.ppm" "$OUT/d.ppm"); echo "noisy (read 2 DN + shot): $a dB, --denoise 1: $b dB"
awk "BEGIN{exit !($b > $a)}" || { echo "FAIL: denoise does not help"; fail=1; }

# all outputs at once; the DNG and JPEG are checked with independent readers when present
$B/rawtool --size 1200x900 --cfa grbg --cell 3 --dng "$OUT/o.dng" --jpeg "$OUT/o.jpg" --png "$OUT/o.png" \
	--ppm "$OUT/o.ppm" --bayer "$OUT/o.pgm" --preview "$OUT/p.jpg" --sharpen 0.3 "$R" 2>/dev/null
$B/rawsynth sensor --in "$OUT/o.png" -o "$OUT/png.raw" --cell 1 --ref "$OUT/png.ppm" 2>/dev/null
$B/rawtool psnr "$OUT/o.ppm" "$OUT/png.ppm" --border 0 | grep -q "all 99.00" && echo "png: ok (reads back identical)" || { echo "FAIL: png"; fail=1; }
if command -v ffmpeg >/dev/null; then
	ffmpeg -loglevel error -y -i "$OUT/o.jpg" -pix_fmt rgb24 "$OUT/jd.ppm"
	v=$(psnr "$OUT/o.ppm" "$OUT/jd.ppm"); echo "jpeg (ffmpeg decode) vs ppm: $v dB"; check jpeg "$v" 32
	ffmpeg -loglevel error -y -i "$OUT/p.jpg" -f null - && echo "preview jpeg: ok"
	ffprobe -hide_banner "$OUT/o.dng" 2>&1 | grep -q "bayer_rggb16le, 1200x900" && echo "dng (ffprobe): ok" || { echo "FAIL: dng"; fail=1; }
fi
if command -v python3 >/dev/null; then
	python3 tests/dngcheck.py "$OUT/o.dng" "$OUT/o.pgm" || fail=1
fi
[ $fail = 0 ] && echo "ALL OK" || { echo "SOME TESTS FAILED"; exit 1; }
