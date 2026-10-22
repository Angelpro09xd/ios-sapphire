#!/bin/bash
# Mesa for sapphire's iOS: the OpenGL ES library behind the EAGL bridge, with freedreno for the
# Adreno 610 (through the SapphireGPU service, xnu patch 0149) and softpipe as a fallback.
# HackiOS's tools/mk-mesa.sh (userland/hackios/README.md), made to build on Linux with LLVM 18:
# clang -target arm64-apple-ios16.0 and ld64.lld against an iPhoneOS 16.5 SDK (theos/sdks, kept
# outside the repository: SDK=...). Out: $B/out/libOSMesa.8.dylib, exported-gl.txt, gl.xml.
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-/serverhive/angelpro09devv/sdks/theos-sdks/iPhoneOS16.5.sdk}
B=${B:-/serverhive/angelpro09devv/mesa-ios}
LLVM=${LLVM:-/usr/lib/llvm-18/bin}
export PATH=$LLVM:$HOME/.local/bin:$PATH
mkdir -p "$B/out" && cd "$B"
PATCHES=$(cat "$HERE"/mesa/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.sapphire-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$HERE"/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.sapphire-patches
fi
rm -rf mesa-$VERSION/src/redwood-drm mesa-$VERSION/src/compiler/air
cp -r "$HERE/mesa/sapphire-drm" mesa-$VERSION/src/redwood-drm
cp -r "$HERE/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH
T="'-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK'"
cat > cross.ini <<INI
[binaries]
c = ['$LLVM/clang', $T]
cpp = ['$LLVM/clang++', $T, '-fuse-cxa-atexit']
objc = ['$LLVM/clang', $T]
ar = '$LLVM/llvm-ar'
ranlib = '$LLVM/llvm-ranlib'
strip = '$LLVM/llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[built-in options]
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld']
objc_link_args = ['-fuse-ld=lld']

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
llvm-strip -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
llvm-install-name-tool -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
llvm-nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml out/gl.xml
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/out/libOSMesa.8.dylib, $(stat -c %s out/libOSMesa.8.dylib) bytes, $(wc -l < out/exported-gl.txt) gl* functions"
