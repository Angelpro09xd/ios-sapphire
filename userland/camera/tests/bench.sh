#!/bin/sh
# Full-size benchmark: a synthetic 108 MP S5KHM6 frame (12000x9000 nonacell RAW10, stride
# 15008 like the TFE writes it) through the whole pipeline. Works on the PC and on the phone
# (copy build/ios/{rawtool,rawsynth} and this script; run with BIN=/path/to/binaries).
#   sh tests/bench.sh [photo.png]     (default: the synthetic "mix" pattern)
set -e
cd "$(dirname "$0")/.."
BIN=${BIN:-build/host}
OUT=${OUT:-/tmp/camtest-$(id -un)}
mkdir -p "$OUT"
if [ -n "$1" ]; then SRC="--in $1"; else SRC="--kind mix"; fi
[ -f "$OUT/hm6.raw" ] || $BIN/rawsynth sensor $SRC --size 12000x9000 --cfa grbg --cell 3 --stride 15008 \
	--noise 1.5 --shot 0.4 -o "$OUT/hm6.raw"
for chunk in 64 32; do
	echo "== chunk $chunk: DNG + JPEG + preview"
	$BIN/rawtool --preset hm6 --chunk $chunk --dng "$OUT/hm6.dng" --jpeg "$OUT/hm6.jpg" --preview "$OUT/hm6-prev.jpg" \
		"$OUT/hm6.raw"
done
echo "== preview only (binned 4000x3000 JPEG)"
$BIN/rawtool --preset hm6 --preview "$OUT/hm6-prev.jpg" "$OUT/hm6.raw"
echo "== fast remosaic (--rm-iter 0), JPEG only"
$BIN/rawtool --preset hm6 --rm-iter 0 --jpeg "$OUT/hm6.jpg" "$OUT/hm6.raw"
ls -l "$OUT"/hm6*
