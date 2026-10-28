#!/bin/bash
# llvmget.sh: llvm + clang + cmake sources of the release matching Ubuntu's clang-18, into ~/xnu-tools/llvm-src
set -e
v=$(clang-18 --version | sed -n 's/.*version \([0-9.]*\).*/\1/p' | head -1)
echo "clang-18 is $v"
D=~/xnu-tools/llvm-src
mkdir -p $D/dl && cd $D/dl
for p in llvm clang cmake third-party; do
	f=$p-$v.src.tar.xz
	[ -s $f ] || curl -sSfL -C - -o $f https://github.com/llvm/llvm-project/releases/download/llvmorg-$v/$f || echo "no $f"
done
ls -la
cd $D
for p in llvm clang cmake third-party; do
	[ -d $p ] || { [ -s dl/$p-$v.src.tar.xz ] && tar xf dl/$p-$v.src.tar.xz && mv $p-$v.src $p; }
done
ls $D
