#!/bin/bash
# Reproducible build of static aarch64 apfs-fuse (+apfsutil) and mkfs.hfsplus
# for OrangeFox recovery. Run inside WSL (no sudo needed):
#   bash /home/angel/ios-sapphire/installer/tools-arm64/build.sh [test]
# Needs: aarch64-linux-gnu-gcc/g++, meson, ninja, cmake, git, curl, patch.
# Optional "test" argument runs the qemu-aarch64 smoke tests at the end.
set -euo pipefail

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DL=$TOP/dl
SRC=$TOP/src
BLD=$TOP/build
PREFIX=$TOP/prefix
OUT=$TOP/out
JOBS=$(nproc)

HOST=aarch64-linux-gnu
CC=$HOST-gcc
CXX=$HOST-g++
AR=$HOST-ar
RANLIB=$HOST-ranlib
STRIP=$HOST-strip
OPT_CFLAGS="-Os -ffunction-sections -fdata-sections"
OPT_LDFLAGS="-static -Wl,--gc-sections"

# ---- pinned versions ----------------------------------------------------
ZLIB_VER=1.3.1
ZLIB_URL=https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

BZIP2_VER=1.0.8
BZIP2_URL=https://sourceware.org/pub/bzip2/bzip2-$BZIP2_VER.tar.gz
BZIP2_SHA=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269

FUSE_VER=3.16.2
FUSE_URL=https://github.com/libfuse/libfuse/releases/download/fuse-$FUSE_VER/fuse-$FUSE_VER.tar.gz
FUSE_SHA=f797055d9296b275e981f5f62d4e32e089614fc253d1ef2985851025b8a0ce87

OPENSSL_VER=3.0.15
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz
OPENSSL_SHA=23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533

HFSPROGS_VER=540.1.linux3
HFSPROGS_DEB=6build1
HFSPROGS_BASE=http://archive.ubuntu.com/ubuntu/pool/universe/h/hfsprogs
HFSPROGS_ORIG_SHA=b01b203a97f9a3bf36a027c13ddfc59292730552e62722d690d33bd5c24f5497
HFSPROGS_DEBIAN_SHA=e5edfafed563ad337e2ca16d8439f0424ecf0e0ac558c2c5896569cf7389f914

APFS_REPO=https://github.com/sgan81/apfs-fuse.git
APFS_COMMIT=66b86bd525e8cb90f9012543be89b1f092b75cf3   # 2023-03-12, master
LZFSE_COMMIT=e634ca58b4821d9f3d560cdc6df5dec02ffc93fd  # submodule pin

# ---- helpers -------------------------------------------------------------
fetch() { # url sha256
	local url=$1 sha=$2 f=$DL/$(basename "$1")
	mkdir -p "$DL"
	[ -f "$f" ] || curl -sSfL -o "$f.part" "$url" && { [ -f "$f" ] || mv "$f.part" "$f"; }
	if [[ $sha != @* ]]; then
		echo "$sha  $f" | sha256sum -c --quiet - || { echo "checksum mismatch: $f"; exit 1; }
	else
		echo "NOTE: unpinned $f sha256=$(sha256sum "$f" | cut -d' ' -f1)"
	fi
}
unpack() { # tarball destdir-name
	rm -rf "$SRC/$2"; mkdir -p "$SRC"
	tar -C "$SRC" -xf "$DL/$1"
}

mkdir -p "$DL" "$SRC" "$BLD" "$PREFIX/lib" "$PREFIX/include" "$OUT"

# ---- zlib ----------------------------------------------------------------
build_zlib() {
	fetch $ZLIB_URL $ZLIB_SHA
	unpack zlib-$ZLIB_VER.tar.gz zlib-$ZLIB_VER
	cd "$SRC/zlib-$ZLIB_VER"
	CC=$CC AR=$AR RANLIB=$RANLIB CFLAGS="$OPT_CFLAGS" CHOST=$HOST \
		./configure --static --prefix="$PREFIX" >/dev/null
	make -j"$JOBS" libz.a >/dev/null
	make install >/dev/null
}

# ---- bzip2 ---------------------------------------------------------------
build_bzip2() {
	fetch $BZIP2_URL $BZIP2_SHA
	unpack bzip2-$BZIP2_VER.tar.gz bzip2-$BZIP2_VER
	cd "$SRC/bzip2-$BZIP2_VER"
	make -j"$JOBS" libbz2.a CC=$CC AR=$AR RANLIB=$RANLIB \
		CFLAGS="$OPT_CFLAGS -D_FILE_OFFSET_BITS=64" >/dev/null
	install -m644 libbz2.a "$PREFIX/lib/"
	install -m644 bzlib.h "$PREFIX/include/"
}

# ---- libfuse 3 -------------------------------------------------------------
build_fuse() {
	fetch $FUSE_URL $FUSE_SHA
	unpack fuse-$FUSE_VER.tar.gz fuse-$FUSE_VER
	cat > "$BLD/aarch64.meson" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'false'

[built-in options]
c_args = ['-DIGNORE_MTAB', '-Os', '-ffunction-sections', '-fdata-sections']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
	rm -rf "$BLD/fuse"
	# -DIGNORE_MTAB (c_args above; the meson disable-mtab option is a no-op in 3.16.2): never try to fork /bin/mount -f to update /etc/mtab
	# (recovery has no /bin/mount; the mount itself is done via mount(2)).
	meson setup "$BLD/fuse" "$SRC/fuse-$FUSE_VER" \
		--cross-file "$BLD/aarch64.meson" --prefix="$PREFIX" --libdir=lib \
		--buildtype=release -Ddefault_library=static \
		-Dutils=false -Dexamples=false -Dtests=false \
		-Duseroot=false -Ddisable-mtab=true -Dudevrulesdir="$PREFIX/udev" \
		-Dinitscriptdir= >/dev/null
	ninja -C "$BLD/fuse" >/dev/null
	ninja -C "$BLD/fuse" install >/dev/null
}

# ---- apfs-fuse -------------------------------------------------------------
build_apfs() {
	local s=$SRC/apfs-fuse
	if [ ! -d "$s/.git" ]; then
		git clone -q "$APFS_REPO" "$s"
	fi
	git -C "$s" fetch -q origin || true
	git -C "$s" checkout -q -f "$APFS_COMMIT"
	git -C "$s" submodule -q update --init
	git -C "$s/3rdparty/lzfse" checkout -q -f "$LZFSE_COMMIT"
	# GCC 15: PList.h uses uint64_t without <cstdint>
	grep -q '<cstdint>' "$s/ApfsLib/PList.h" || \
		sed -i 's/#include <memory>/#include <memory>\n#include <cstdint>/' "$s/ApfsLib/PList.h"

	cat > "$BLD/aarch64.cmake" <<EOF
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
EOF
	rm -rf "$BLD/apfs-fuse"
	cmake -S "$s" -B "$BLD/apfs-fuse" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$BLD/aarch64.cmake" \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-DUSE_FUSE3=ON \
		-DCMAKE_C_FLAGS="$OPT_CFLAGS -I$PREFIX/include" \
		-DCMAKE_CXX_FLAGS="$OPT_CFLAGS -I$PREFIX/include" \
		-DCMAKE_EXE_LINKER_FLAGS="$OPT_LDFLAGS -L$PREFIX/lib" >/dev/null
	# CMakeLists forces CMAKE_BUILD_TYPE=Release (-O3); fine.
	ninja -C "$BLD/apfs-fuse" apfs-fuse apfsutil
	for b in apfs-fuse apfsutil; do
		$STRIP -o "$OUT/$b" "$BLD/apfs-fuse/$b"
	done
}

# ---- OpenSSL (libcrypto only; newfs_hfs uses SHA1 for the volume UUID) ----
build_openssl() {
	fetch $OPENSSL_URL $OPENSSL_SHA
	unpack openssl-$OPENSSL_VER.tar.gz openssl-$OPENSSL_VER
	cd "$SRC/openssl-$OPENSSL_VER"
	./Configure linux-aarch64 --cross-compile-prefix=$HOST- \
		--prefix="$PREFIX" --libdir=lib no-shared no-asm no-tests \
		no-engine no-dso no-module $OPT_CFLAGS >/dev/null
	make -j"$JOBS" build_generated >/dev/null
	make -j"$JOBS" libcrypto.a >/dev/null
	install -m644 libcrypto.a "$PREFIX/lib/"
	mkdir -p "$PREFIX/include/openssl"
	install -m644 include/openssl/*.h "$PREFIX/include/openssl/"
}

# ---- mkfs.hfsplus (newfs_hfs from hfsprogs) --------------------------------
build_hfs() {
	fetch $HFSPROGS_BASE/hfsprogs_$HFSPROGS_VER.orig.tar.gz $HFSPROGS_ORIG_SHA
	fetch $HFSPROGS_BASE/hfsprogs_$HFSPROGS_VER-$HFSPROGS_DEB.debian.tar.xz $HFSPROGS_DEBIAN_SHA
	local s=$SRC/diskdev_cmds-$HFSPROGS_VER
	unpack hfsprogs_$HFSPROGS_VER.orig.tar.gz diskdev_cmds-$HFSPROGS_VER
	rm -rf "$SRC/hfsprogs-debian"; mkdir -p "$SRC/hfsprogs-debian"
	tar -C "$SRC/hfsprogs-debian" -xf "$DL/hfsprogs_$HFSPROGS_VER-$HFSPROGS_DEB.debian.tar.xz"
	cd "$s"
	while read -r p; do
		[ -n "$p" ] && [[ $p != \#* ]] || continue
		patch -p1 -s < "$SRC/hfsprogs-debian/debian/patches/$p"
	done < "$SRC/hfsprogs-debian/debian/patches/series"
	cd newfs_hfs.tproj
	local cf="$OPT_CFLAGS -Wall -I$s/include -I$PREFIX/include -DDEBUG_BUILD=0 \
		-D_FILE_OFFSET_BITS=64 -DLINUX=1 -DBSD=1 -DVERSION=\"$HFSPROGS_VER\" \
		-Wno-deprecated-declarations"
	$CC $cf -o newfs_hfs hfs_endian.c makehfs.c newfs_hfs.c \
		$OPT_LDFLAGS -L"$PREFIX/lib" -lcrypto
	$STRIP -o "$OUT/mkfs.hfsplus" newfs_hfs
}

# ---- qemu smoke tests (no root needed) ------------------------------------
QEMU_DEB_VER=1:10.2.1+ds-1ubuntu3.3
get_qemu() {
	local q=$TOP/qemu/usr/bin/qemu-aarch64
	if [ ! -x "$q" ]; then
		( cd "$DL" && apt-get download "qemu-user=$QEMU_DEB_VER" >/dev/null 2>&1 || apt-get download qemu-user >/dev/null )
		mkdir -p "$TOP/qemu"; dpkg-deb -x "$DL"/qemu-user_*_amd64.deb "$TOP/qemu"
	fi
	echo "$q"
}
run_tests() {
	local q; q=$(get_qemu)
	local t=$BLD/test; rm -rf "$t"; mkdir -p "$t"
	echo "== apfs-fuse usage (qemu)"; "$q" "$OUT/apfs-fuse" 2>&1 | head -3 || true
	echo "== apfsutil usage (qemu)";  "$q" "$OUT/apfsutil" 2>&1 | head -3 || true
	echo "== mkfs.hfsplus (qemu) vs host mkfs"
	truncate -s 64M "$t/arm.img" "$t/host.img"
	"$q" "$OUT/mkfs.hfsplus" -s -J -v TestVol "$t/arm.img"
	/sbin/mkfs.hfsplus -s -J -v TestVol "$t/host.img" >/dev/null
	echo "-- fsck arm-made:";  /sbin/fsck.hfsplus -fn "$t/arm.img"  || true
	echo "-- fsck host-made:"; /sbin/fsck.hfsplus -fn "$t/host.img" || true
}

case "${1:-all}" in
	test) run_tests; exit 0 ;;
esac

build_zlib
build_bzip2
build_fuse
build_apfs
build_openssl
build_hfs
ls -l "$OUT"
file "$OUT"/*
[ "${1:-}" = "all+test" ] && run_tests
exit 0
