#!/bin/bash
# mkld64.sh: Apple's cctools ld64 and strip for Linux, from PureDarwin's sources (their nix/pkgs/toolchain/native-ld.nix
# without Nix), into ~/xnu-tools/bin. The SDK is our iPhoneOS 16.5 one (Darwin/Mach headers).
set -e
PD=~/tools-dl/pd/PureDarwin
W=~/xnu-tools/native-ld
SDK=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk
T=x86_64-apple-darwin20.4
rm -rf $W && mkdir -p $W ~/xnu-tools/bin
cd $W
mkdir -p cmake tools/cctools src/Libraries/libSystem
cp $PD/cmake/suppress_warnings.cmake cmake/
cp -r $PD/tools/xar tools/
for d in include libmacho libstuff misc ld64; do cp -r $PD/tools/cctools/$d tools/cctools/; done
cp -r $PD/src/Libraries/CommonCrypto src/Libraries/
cp -r $PD/src/Libraries/libSystem/corecrypto src/Libraries/libSystem/
mkdir -p .stubs .native-stubs/sdk-shim-include
printf '#!/bin/sh\necho 11.3\n' > .stubs/sw_vers; chmod +x .stubs/sw_vers
export PATH="$PWD/.stubs:$PATH"
ln -sf /usr/bin/clang-18 .native-stubs/$T-clang
ln -sf "$(command -v ar)" .native-stubs/$T-ar
echo '#include <machine/endian.h>' > .native-stubs/sdk-shim-include/endian.h
export DARWIN_SDK_ROOT=$SDK
export NIX_NATIVE_DARWIN_HEADER_FLAGS="-isysroot $SDK -D__APPLE__ -D__MACH__ -D__APPLE_CC__=1 -D__arm64__=1"
export NIX_NATIVE_DARWIN_HEADER_DIRS="$PWD/.native-stubs/sdk-shim-include;$SDK/usr/include"
export NIX_DARWIN_TOOLCHAIN_DIR="$PWD/.native-stubs"
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.15.1)
project(PUREDARWIN_NATIVE_LD)

include(cmake/suppress_warnings.cmake)
add_compile_options(-Wno-return-type -Wno-error=cpp -Wno-nullability-completeness)

add_subdirectory(src/Libraries/libSystem/corecrypto)
add_subdirectory(src/Libraries/CommonCrypto)
add_subdirectory(tools/xar)
add_subdirectory(tools/cctools/libmacho)
add_subdirectory(tools/cctools/libstuff)
add_subdirectory(tools/cctools/misc)
add_subdirectory(tools/cctools/ld64/src)
EOF
echo 'include(host_corecrypto_static.cmake)' > src/Libraries/libSystem/corecrypto/CMakeLists.txt
echo 'include(host_commoncrypto_static.cmake)' > src/Libraries/CommonCrypto/CMakeLists.txt
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
	-DCMAKE_AR="$PWD/.native-stubs/$T-ar" -DCMAKE_BUILD_TYPE=Release -DPUREDARWIN_MACOSX_SDK="$SDK" > cmake.log 2>&1 || { tail -30 cmake.log; exit 1; }
ninja -C build host_ld host_strip > ninja.log 2>&1 || { grep -m 20 -e 'error' -e 'Error' ninja.log; tail -5 ninja.log; exit 1; }
cp build/tools/cctools/ld64/src/ld/ld ~/xnu-tools/bin/ld.real
printf '#!/bin/sh\nulimit -s unlimited 2>/dev/null || true\nexec "$(dirname "$0")/ld.real" "$@"\n' > ~/xnu-tools/bin/ld
chmod +x ~/xnu-tools/bin/ld ~/xnu-tools/bin/ld.real
cp build/tools/cctools/misc/strip ~/xnu-tools/bin/strip
~/xnu-tools/bin/ld -v 2>&1 | head -3
