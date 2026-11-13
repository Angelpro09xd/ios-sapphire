H=/home/angel/ios-sapphire/installer/tools-arm64/heic
X=$H/out/sapphire-heic2png.x86
Q=$H/../qemu/usr/bin/qemu-aarch64
$X; echo "rc=$?"
$X /nonexistent.heic /tmp/x.png; echo "rc=$?"
$X $H/build.sh /tmp/x.png; echo "rc=$?"
$X "$H/testdata/7400.WWDC_2022_Background-375w-812h@3x~iphone.ca/assets/Head.HEIC" /nonexistent-dir/x.png; echo "rc=$?"
$Q $H/out/sapphire-heic2png /nonexistent.heic /tmp/x.png; echo "rc=$?"
ls -l $H/out; sha256sum $H/out/*
