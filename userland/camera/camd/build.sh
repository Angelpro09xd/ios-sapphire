#!/bin/bash
# build.sh [OUTDIR]: the camera daemon for the phone
#   camd     -> /redwood/bin/camd     (launchd job com.sapphire.camd, com.sapphire.camd.plist)
#   camctl   -> /redwood/bin/camctl   (camd from the shell)
# build.sh test: the preview renderer and the develop step on the PC (rawsynth frames, ../build/host)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
CLANG=${CLANG:-/usr/lib/llvm-18/bin/clang}
LIB="../util.c ../stage.c ../rawio.c ../remosaic.c ../demosaic.c ../color.c ../out.c ../jpeg.c"
OWN="preview.c develop.c exif.c"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"

if [ "$1" = test ]; then
	out=${OUT:-/tmp/camd-test-$(id -un)}
	mkdir -p "$out"
	$CLANG -O2 -std=gnu11 -march=native $WARN -o "$out/hosttest" hosttest.c $OWN $LIB -lm -lpthread
	(cd .. && make build/host/rawsynth >/dev/null)
	syn=../build/host/rawsynth
	for t in "1280x720 1 90" "4000x3000 1 90" "4000x3000 3 270"; do
		set -- $t
		$syn sensor --kind natural --size "$1" --cfa grbg --cell "$2" -o "$out/f.raw" >/dev/null 2>&1 ||
			$syn sensor --kind natural --size "$1" --cfa grbg --cell "$2" -o "$out/f.raw"
		w=${1%x*} h=${1#*x}
		echo "== $1 cell $2 rotate $3"
		"$out/hosttest" "$out/f.raw" "$w" "$h" "$2" "$3" "$out/pv-$1-$2.ppm" "$out/still-$1-$2.jpg" "$out/still-$1-$2.dng" | grep -v '^P '
	done
	ls -l "$out"
	exit 0
fi

out=${1:-/tmp}
CC="$CLANG -target arm64-apple-ios15.0 -isysroot $SDK -fuse-ld=lld"
$CC -O3 -std=gnu11 -mcpu=cortex-a73 -ffp-contract=fast -fno-math-errno $WARN -o "$out/camd" camd.c camk.c $OWN $LIB
$CC -O2 -Wall -o "$out/camctl" camctl.c
for f in camd camctl; do "$LDID" -S "$out/$f"; done
cp com.sapphire.camd.plist "$out/"
echo "$out: camd camctl com.sapphire.camd.plist"
