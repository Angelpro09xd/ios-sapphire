#!/bin/bash
# mkclang.sh: clang 18.1.8 with the XNU type builtins (tools/xnu-linux/clang-xnu-builtins.py), AArch64 only,
# into ~/xnu-tools/llvm-build (bin/clang)
set -e
S=~/xnu-tools/llvm-src
B=~/xnu-tools/llvm-build
python3 "$(dirname "$0")/clang-xnu-builtins.py" $S/clang
mkdir -p $B
[ -f $B/build.ninja ] || cmake -S $S/llvm -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_USE_LINKER=lld \
	-DCMAKE_CXX_FLAGS='-include cstdint' \
	-DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_ASSERTIONS=OFF \
	-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DCLANG_INCLUDE_TESTS=OFF \
	-DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF > $B/../cmake-llvm.log 2>&1 || { tail -20 $B/../cmake-llvm.log; exit 1; }
time ninja -C $B clang > $B/../ninja-llvm.log 2>&1 || { grep -m 20 'error' $B/../ninja-llvm.log; exit 1; }
$B/bin/clang --version | head -1
