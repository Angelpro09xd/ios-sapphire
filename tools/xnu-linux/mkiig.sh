#!/bin/bash
# mkiig.sh: PureDarwin's iig-lite (DriverKit .iig to C++), with iigfix.py, into ~/xnu-tools/bin/iig
set -e
S=$(cd "$(dirname "$0")" && pwd)
cd ~/tools-dl/pd/iig-tools
python3 $S/iigfix.py .
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release > /dev/null
ninja -C build 2>&1 | tail -1
cp build/iig ~/xnu-tools/bin/iig
