#!/bin/bash
# The EAGL bridge (HackiOS's eagl.c, userland/hackios/) over our Mesa: obj/libredwood-gl.dylib.
set -e
cd "$(dirname "$0")"
M=${M:-/serverhive/angelpro09devv/mesa-ios/out}
SDK=${SDK:-/serverhive/angelpro09devv/sdks/theos-sdks/iPhoneOS16.5.sdk}
L=${LLVM:-/usr/lib/llvm-18/bin}
export PATH=$L:$PATH
mkdir -p obj
python3 mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -Wno-unused-function -Wno-frame-address \
	-dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation -framework IOKit \
	-framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-gl.dylib src/eagl.c src/gl_osmesa.c
echo "obj/libredwood-gl.dylib"
