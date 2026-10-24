#!/bin/bash
# iOS's sound on the phone (kernel 0200+, audio=2): RedwoodAudio.driver as the only HAL plug-in, and
# VirtualAudio.plugin where mediaserverd looks for it (it had been moved to HAL.disabled; without it
# mediaserverd says "No Audio Device Available" and routes nothing). Run from WSL (tools/bin/wsl).
set -e
cd "$(dirname "$0")/.."
userland/audio/build.sh >/dev/null
W=/serverhive/angelpro09devv/tools/bin/wsl
$W 'cat > /tmp/RedwoodAudio' < userland/obj/RedwoodAudio.driver/RedwoodAudio
$W 'cat > /tmp/RedwoodAudio.plist' < userland/obj/RedwoodAudio.driver/Info.plist
$W 'K="-o ConnectTimeout=10 -i ~/.ssh/sapphire_phone"; P=root@192.168.7.2
ssh $K $P "mkdir -p /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver /System/Library/Audio/Plug-Ins/HAL.apple"
scp -q $K /tmp/RedwoodAudio $P:/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/RedwoodAudio
scp -q $K /tmp/RedwoodAudio.plist $P:/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/Info.plist
ssh $K $P "cd /System/Library/Audio/Plug-Ins; for d in HAL/*.driver; do [ \$d = HAL/RedwoodAudio.driver ] || mv \$d HAL.apple/; done
[ -d /Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin ] && mv /Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin /Library/Audio/Plug-Ins/HAL/
ls /System/Library/Audio/Plug-Ins/HAL /Library/Audio/Plug-Ins/HAL; killall -9 mediaserverd; echo installed"'
