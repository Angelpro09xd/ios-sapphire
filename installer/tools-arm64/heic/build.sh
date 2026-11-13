#!/bin/bash
# Reproducible build of sapphire-heic2png (HEIC -> PNG) as a fully static,
# stripped executable for aarch64 (OrangeFox recovery) and x86_64 (PC tests).
# Run inside WSL (no sudo):
#   bash /home/angel/ios-sapphire/installer/tools-arm64/heic/build.sh [arm64|x86|all] [test]
# Needs: aarch64-linux-gnu-gcc/g++, gcc/g++, cmake, ninja, curl.
# aarch64 libs go to ../prefix (shared with ../build.sh, zlib reused from there),
# x86_64 libs go to ./prefix-x86.
set -euo pipefail

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DL=$TOP/dl
SRC=$TOP/src
BLD=$TOP/build
OUT=$TOP/out
JOBS=$(nproc)
OPT_CFLAGS="-Os -ffunction-sections -fdata-sections"
OPT_LDFLAGS="-static -Wl,--gc-sections"

# ---- pinned versions ----------------------------------------------------
ZLIB_VER=1.3.1
ZLIB_URL=https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

DE265_VER=1.0.15
DE265_URL=https://github.com/strukturag/libde265/releases/download/v$DE265_VER/libde265-$DE265_VER.tar.gz
DE265_SHA=00251986c29d34d3af7117ed05874950c875dd9292d016be29d3b3762666511d

HEIF_VER=1.18.2
HEIF_URL=https://github.com/strukturag/libheif/releases/download/v$HEIF_VER/libheif-$HEIF_VER.tar.gz
HEIF_SHA=c4002a622bec9f519f29d84bfdc6024e33fd67953a5fb4dc2c2f11f67d5e45bf

# ---- helpers -------------------------------------------------------------
fetch() { # url sha256
	local url=$1 sha=$2 f=$DL/$(basename "$1")
	mkdir -p "$DL"
	if [ ! -f "$f" ]; then
		# reuse the parent tree's download cache when it has the file
		if [ -f "$TOP/../dl/$(basename "$1")" ]; then cp "$TOP/../dl/$(basename "$1")" "$f"
		else curl -sSfL -o "$f.part" "$url" && mv "$f.part" "$f"; fi
	fi
	echo "$sha  $f" | sha256sum -c --quiet - || { echo "checksum mismatch: $f"; exit 1; }
}
unpack() { # tarball dirname
	rm -rf "$SRC/$2"; mkdir -p "$SRC"
	tar -C "$SRC" -xf "$DL/$1"
}

# per-arch settings: sets ARCH CC CXX AR RANLIB STRIP PREFIX TC SUFFIX
setarch() {
	ARCH=$1
	if [ "$ARCH" = arm64 ]; then
		local H=aarch64-linux-gnu
		CC=$H-gcc CXX=$H-g++ AR=$H-ar RANLIB=$H-ranlib STRIP=$H-strip
		PREFIX=$(cd "$TOP/.." && pwd)/prefix SUFFIX= CHOST=$H
		TC=$BLD/aarch64.cmake
		mkdir -p "$BLD"
		cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_AR $(command -v $AR))
set(CMAKE_RANLIB $(command -v $RANLIB))
set(CMAKE_FIND_ROOT_PATH $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
	else
		CC=gcc CXX=g++ AR=ar RANLIB=ranlib STRIP=strip
		PREFIX=$TOP/prefix-x86 SUFFIX=.x86 CHOST=
		TC=$BLD/x86.cmake
		mkdir -p "$BLD"
		cat > "$TC" <<EOF
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_FIND_ROOT_PATH $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
	fi
	mkdir -p "$PREFIX/lib" "$PREFIX/include" "$OUT"
}

# ---- zlib (only if the prefix lacks it; arm64 prefix already has it) -------
build_zlib() {
	[ -f "$PREFIX/lib/libz.a" ] && [ -f "$PREFIX/include/zlib.h" ] && return 0
	fetch $ZLIB_URL $ZLIB_SHA
	local s=$SRC/zlib-$ZLIB_VER-$ARCH
	rm -rf "$s"; mkdir -p "$s"; tar -C "$s" --strip-components=1 -xf "$DL/zlib-$ZLIB_VER.tar.gz"
	cd "$s"
	CC=$CC AR=$AR RANLIB=$RANLIB CFLAGS="$OPT_CFLAGS" CHOST=$CHOST \
		./configure --static --prefix="$PREFIX" >/dev/null
	make -j"$JOBS" libz.a >/dev/null
	make install >/dev/null
}

# ---- libde265 (HEVC decoder only) ------------------------------------------
build_de265() {
	fetch $DE265_URL $DE265_SHA
	[ -d "$SRC/libde265-$DE265_VER" ] || unpack libde265-$DE265_VER.tar.gz libde265-$DE265_VER
	rm -rf "$BLD/de265-$ARCH"
	cmake -S "$SRC/libde265-$DE265_VER" -B "$BLD/de265-$ARCH" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
		-DBUILD_SHARED_LIBS=OFF -DENABLE_SDL=OFF \
		-DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF \
		-DCMAKE_C_FLAGS_RELEASE="$OPT_CFLAGS -DNDEBUG" \
		-DCMAKE_CXX_FLAGS_RELEASE="$OPT_CFLAGS -DNDEBUG" >/dev/null
	ninja -C "$BLD/de265-$ARCH" >/dev/null
	ninja -C "$BLD/de265-$ARCH" install >/dev/null
}

# ---- libheif (decode only, builtin libde265, no plugins) -------------------
build_heif() {
	fetch $HEIF_URL $HEIF_SHA
	[ -d "$SRC/libheif-$HEIF_VER" ] || unpack libheif-$HEIF_VER.tar.gz libheif-$HEIF_VER
	rm -rf "$BLD/heif-$ARCH"
	cmake -S "$SRC/libheif-$HEIF_VER" -B "$BLD/heif-$ARCH" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
		-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
		-DENABLE_PLUGIN_LOADING=OFF -DENABLE_MULTITHREADING_SUPPORT=ON \
		-DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF \
		-DLIBDE265_INCLUDE_DIR="$PREFIX/include" -DLIBDE265_LIBRARY="$PREFIX/lib/libde265.a" \
		-DWITH_X265=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF \
		-DWITH_DAV1D=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_KVAZAAR=OFF \
		-DWITH_FFMPEG_DECODER=OFF -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF \
		-DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF \
		-DWITH_OPENJPH_ENCODER=OFF -DWITH_UNCOMPRESSED_CODEC=OFF \
		-DWITH_HEADER_COMPRESSION=OFF -DWITH_LIBSHARPYUV=OFF \
		-DWITH_GDK_PIXBUF=OFF -DWITH_EXAMPLES=OFF -DWITH_FUZZERS=OFF \
		-DCMAKE_C_FLAGS_RELEASE="$OPT_CFLAGS -DNDEBUG" \
		-DCMAKE_CXX_FLAGS_RELEASE="$OPT_CFLAGS -DNDEBUG" > "$BLD/heif-$ARCH.cmake.log"
	grep -E 'libde265|x265|aom|dav1d|plugin' -i "$BLD/heif-$ARCH.cmake.log" | grep -v '^--   *$' | head -20 || true
	ninja -C "$BLD/heif-$ARCH" >/dev/null
	ninja -C "$BLD/heif-$ARCH" install >/dev/null
}

# ---- the tool --------------------------------------------------------------
build_tool() {
	local o=$BLD/sapphire-heic2png-$ARCH
	$CC $OPT_CFLAGS -Wall -Wextra -Wno-sign-compare -std=gnu11 \
		-I"$PREFIX/include" -c "$TOP/sapphire-heic2png.c" -o "$o.o"
	$CXX $OPT_LDFLAGS -o "$o" "$o.o" -L"$PREFIX/lib" -lheif -lde265 -lz -lpthread
	$STRIP -o "$OUT/sapphire-heic2png$SUFFIX" "$o"
}

build_arch() {
	setarch "$1"
	echo "=== $ARCH (prefix $PREFIX)"
	build_zlib
	build_de265
	build_heif
	build_tool
}

# ---- tests -----------------------------------------------------------------
run_tests() {
	local x=$OUT/sapphire-heic2png.x86 a=$OUT/sapphire-heic2png
	local q=$TOP/../qemu/usr/bin/qemu-aarch64 t=$TOP/testout
	rm -rf "$t"; mkdir -p "$t"
	(cd "$TOP/testdata" && find . -iname '*.heic' | sort) | while read -r f; do
		local o="$t/${f%.*}.png"; mkdir -p "$(dirname "$o")"
		echo "--- $f"
		"$x" --info "$TOP/testdata/$f"
		"$x" "$TOP/testdata/$f" "$o"
	done
	local one; one=$(cd "$TOP/testdata" && find . -iname 'Head Mask.HEIC' | head -1)
	[ -n "$one" ] || one=$(cd "$TOP/testdata" && find . -iname '*.heic' | head -1)
	echo "--- qemu-aarch64: $one"
	"$q" "$a" --info "$TOP/testdata/$one"
	"$q" "$a" "$TOP/testdata/$one" "$t/qemu-arm64.png"
	cmp "$t/qemu-arm64.png" "$t/${one%.*}.png" && echo "arm64 output identical to x86 output"
	python3 -I "$TOP/verify.py" "$t" "$TOP/testdata"
}

WHAT=${1:-all}
case "$WHAT" in
	arm64|x86) build_arch "$WHAT" ;;
	all) build_arch x86; build_arch arm64 ;;
	test) ;;
	*) echo "usage: $0 [arm64|x86|all|test] [test]"; exit 2 ;;
esac
if [ "$WHAT" != test ]; then
	ls -l "$OUT"; file "$OUT"/*
fi
if [ "$WHAT" = test ] || [ "${2:-}" = test ]; then run_tests; fi
exit 0
