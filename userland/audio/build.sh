#!/bin/bash
# RedwoodAudio.driver (HackiOS's HAL plug-in, from ios-topaz userland/audio, MIT): the output device
# of iOS, writing to /dev/redwood-audio (kernel 0200). Install on the phone as
# /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/{RedwoodAudio,Info.plist}, then restart mediaserverd.
set -e
cd "$(dirname "$0")"
SDK=${SDK:-/serverhive/angelpro09devv/sdks/theos-sdks/iPhoneOS16.5.sdk}
L=${LLVM:-/usr/lib/llvm-18/bin}
mkdir -p ../obj/RedwoodAudio.driver
$L/clang -target arm64-apple-ios16.0 -isysroot $SDK -fuse-ld=lld -O2 -Wall -bundle -framework CoreFoundation \
	-o ../obj/RedwoodAudio.driver/RedwoodAudio audio_plugin.c
cp audio_plugin.plist ../obj/RedwoodAudio.driver/Info.plist
echo "../obj/RedwoodAudio.driver"
