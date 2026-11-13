# 0.1.3: VirtualAudio back in HAL (installs up to 0.1.2 moved it to HAL.disabled: iOS had no audio output at all)
d=/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin
h=/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin
if [ -d $d ] && [ ! -e $h ]; then
	mv $d $h && echo "VirtualAudio enabled again" && killall -9 mediaserverd 2>/dev/null
fi
true
