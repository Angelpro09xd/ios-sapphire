#!/bin/bash
# fptest.sh STEP [PHONE]: the fingerprint unlock test plan, run from WSL (phone root@192.168.7.2).
#
# Needs: a kernel with 0261 (kern.sweet_qsee ops 26/27, kern.sweet_fod_*) booted with qsee=1, a passcode set,
# the enrolled finger in /var/root/fp (finger_0_0.so, auth_token_0.so, the gf_* factory files).
#
#   1. fptest.sh build       builds everything (no phone)
#   2. fptest.sh install     install.sh: binaries, tweak, job added to launchd's cache (backup launchd.plist.pre-fpd)
#   3. fptest.sh start       starts fpd now, without a reboot (launchctl bootstrap / load; else nohup)
#      fptest.sh status      job, process, kernel state, the last log lines
#   4. fptest.sh respring    restarts SpringBoard so the tweak loads (the phone comes back locked)
#   5. On the phone: lock (power button), wake the screen, unlock once with the passcode if this boot is
#      before the first unlock, lock again, wake: the glyph shows over the sensor. Finger on the glyph:
#      the spot lights, the lock screen goes away without the keypad. Wrong finger: the glyph shakes;
#      5 wrong ones: the glyph goes away until the passcode is entered. Swipe up to the keypad: the glyph hides.
#      fptest.sh logs        follow /var/root/fp/fpd.log
#      fptest.sh sblog [S]   SpringBoard's SapphireFingerprint lines for S seconds (oslog, default 30)
#      UI alone, no sensor:  fptest.sh sim-glyph | sim-shake | sim-unlock (needs notifyutil on the phone;
#                            sim-unlock only dismisses the lock screen if the keybag is already unlocked)
#   6. fptest.sh stop        stops fpd for this boot (the cache entry stays: back at the next boot)
#   7. fptest.sh uninstall   uninstall.sh (job out of the cache, tweak and binaries removed, SpringBoard restarted)
#      Worse case: SpringBoard misbehaves -> ssh in, `touch /var/jb/var/sapphire-notweaks; killall -9 SpringBoard`
#      (all tweaks off), or rm /var/jb/usr/lib/TweakInject/SapphireFingerprint.*; launchd cache from before:
#      /System/Library/xpc/launchd.plist.pre-fpd (uninstall.sh --restore-backup).
set -e
cd "$(dirname "$0")"
step=${1:-help}
P=${2:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
phone() { ssh $O $P /var/jb/bin/bash -s "$@"; }
HDR='export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin'

case $step in
build)
	d=${OUT:-$HOME/fp-out}; mkdir -p "$d"; bash build.sh "$d" ;;
install)
	bash install.sh "$P" ;;
uninstall)
	bash uninstall.sh "$P" ;;
start)
	phone <<EOF
$HDR
J=/redwood/share/fpd/com.sapphire.fpd.plist
if launchctl print system/com.sapphire.fpd >/dev/null 2>&1; then
	launchctl kickstart -k system/com.sapphire.fpd && echo "job restarted (kickstart)"
elif launchctl bootstrap system \$J 2>/dev/null; then echo "job loaded (bootstrap)"
elif launchctl load \$J 2>/dev/null; then echo "job loaded (load)"
else
	echo "launchctl could not load the job: running fpd by hand (not restarted if it dies)"
	nohup /redwood/bin/fpd >> /var/tmp/fpd.out 2>&1 &
fi
sleep 3; pgrep -l fpd || echo "fpd not running (exited: see the log)"; tail -15 /var/root/fp/fpd.log
EOF
	;;
stop)
	phone <<EOF
$HDR
launchctl bootout system/com.sapphire.fpd 2>/dev/null || launchctl unload /redwood/share/fpd/com.sapphire.fpd.plist 2>/dev/null || true
killall fpd 2>/dev/null || true; sleep 1; pgrep -l fpd || echo "fpd stopped"
EOF
	;;
status)
	phone <<EOF
$HDR
launchctl print system/com.sapphire.fpd 2>/dev/null | grep -E "state|pid|last exit" || launchctl list | grep fpd || echo "job not loaded"
pgrep -l fpd || echo "fpd not running"
for s in kern.sweet_qsee kern.sweet_fod_press kern.sweet_fod_seq; do echo "\$s = \$(sysctl -n \$s 2>&1)"; done
sysctl -n kern.sweet_qsee_files 2>/dev/null | head -20
/var/root/mkb 2>/dev/null | head -5
ls -l /var/jb/usr/lib/TweakInject/SapphireFingerprint.* /System/Library/xpc/launchd.plist*
tail -25 /var/root/fp/fpd.log 2>/dev/null; tail -5 /var/tmp/fpd.out 2>/dev/null
EOF
	;;
logs)
	phone <<<"tail -n 40 -f /var/root/fp/fpd.log" ;;
sblog)
	phone "${3:-30}" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
oslog --debug > /var/tmp/sfp.all 2>&1 &
sleep "$1"; killall oslog 2>/dev/null
sed 's/\x1b\[[0-9;]*m//g' /var/tmp/sfp.all | grep -a "SapphireFingerprint\|com.sapphire.fpd" | cut -c1-240 | tail -60
EOF
	;;
respring)
	phone <<<"$HDR; killall -9 SpringBoard; echo SpringBoard restarted" ;;
sim-glyph)
	phone <<<"$HDR; notifyutil -s com.sapphire.fp.state 1 && notifyutil -p com.sapphire.fp.state && echo 'state 1 posted (fpd running overrides it)'" ;;
sim-shake)
	phone <<<"$HDR; notifyutil -p com.sapphire.fp.nomatch" ;;
sim-unlock)
	phone <<<"$HDR; notifyutil -p com.sapphire.fp.unlocked" ;;
*)
	sed -n '2,32p' "$0" ;;
esac
