#!/bin/bash
# mktools.sh: PureDarwin and iig-tools sources into ~/tools-dl/pd, this directory's bin/ wrappers (xcrun,
# xclang, install, plutil) into ~/xnu-tools/bin
set -e
S=$(cd "$(dirname "$0")" && pwd)
mkdir -p ~/tools-dl/pd ~/xnu-tools/bin
cd ~/tools-dl/pd
[ -d PureDarwin ] || git clone -q --depth 1 https://github.com/PureDarwin/PureDarwin.git
[ -d iig-tools ] || git clone -q --depth 1 https://github.com/PureDarwin/iig-tools.git
cp $S/bin/xcrun $S/bin/xclang $S/bin/install $S/bin/plutil ~/xnu-tools/bin/
ln -sf xclang ~/xnu-tools/bin/xclang++
chmod +x ~/xnu-tools/bin/*
ls ~/xnu-tools/bin
