#!/bin/bash
# userland/heif/build-libs.sh: libde265 + libheif (decoding only) as static libraries for iOS arm64
# (clang + lld against the iPhoneOS 16.5 SDK), for SapphireHEIF. Out: $P/lib/{libde265,libheif}.a, $P/include.
set -euo pipefail
TOP=$(cd "$(dirname "$0")" && pwd)
SRCS=$TOP/../../installer/tools-arm64/heic/dl
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
L=/usr/lib/llvm-18/bin
B=$TOP/build
P=$TOP/prefix
mkdir -p $B $P
TC=$B/ios.cmake
cat > $TC <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_SYSROOT $SDK)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_C_COMPILER $L/clang)
set(CMAKE_CXX_COMPILER $L/clang++)
set(CMAKE_AR $L/llvm-ar)
set(CMAKE_RANLIB $L/llvm-ranlib)
set(CMAKE_C_COMPILER_TARGET arm64-apple-ios16.0)
set(CMAKE_CXX_COMPILER_TARGET arm64-apple-ios16.0)
set(CMAKE_C_FLAGS_INIT "-isysroot $SDK -O2 -fvisibility=hidden")
set(CMAKE_CXX_FLAGS_INIT "-isysroot $SDK -O2 -fvisibility=hidden -fvisibility-inlines-hidden -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH $P)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
EOF
rm -rf $B/src && mkdir -p $B/src
tar -C $B/src -xf $SRCS/libde265-1.0.15.tar.gz
tar -C $B/src -xf $SRCS/libheif-1.18.2.tar.gz

cmake -S $B/src/libde265-1.0.15 -B $B/de265 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=$P -DBUILD_SHARED_LIBS=OFF -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON > $B/de265.log 2>&1
ninja -C $B/de265 install >> $B/de265.log 2>&1

cmake -S $B/src/libheif-1.18.2 -B $B/heif -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=$P -DBUILD_SHARED_LIBS=OFF -DWITH_LIBDE265=ON -DWITH_X265=OFF -DWITH_AOM_DECODER=OFF \
	-DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_JPEG_DECODER=OFF \
	-DWITH_JPEG_ENCODER=OFF -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF -DWITH_FFMPEG_DECODER=OFF \
	-DWITH_KVAZAAR=OFF -DWITH_UVG266=OFF -DWITH_VVDEC=OFF -DWITH_VVENC=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF \
	-DWITH_LIBSHARPYUV=OFF -DENABLE_PLUGIN_LOADING=OFF -DBUILD_TESTING=OFF -DWITH_UNCOMPRESSED_CODEC=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DLIBDE265_INCLUDE_DIR=$P/include -DLIBDE265_LIBRARY=$P/lib/libde265.a \
	> $B/heif.log 2>&1
ninja -C $B/heif install >> $B/heif.log 2>&1
ls -la $P/lib/*.a
