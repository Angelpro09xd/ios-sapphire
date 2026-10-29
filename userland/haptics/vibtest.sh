#!/bin/bash
# vibtest.sh STEP [PHONE]: the vibration test plan, from WSL (phone root@192.168.7.2). Needs kernel 0263.
#   vibtest.sh install        install.sh: vibd + its launchd cache job + SapphireHaptics (rerun = update);
#                             then ../fingerprint/update.sh for fpd's haptics and the Huella app's switches
#   vibtest.sh status         vibd running, the sysctls, vibd.log
#   vibtest.sh preset NAME    one preset (tap light medium heavy selection keyboard soft rigid long success
#                             warning error notify ring) -- vibtest.sh all: each in turn
#   vibtest.sh ring | stop    the call ring loop (stops by itself after 60 s)
#   On the phone: toggles / pickers / long-press menus / pull-to-refresh vibrate (System Haptics on);
#   keyboard taps vibrate with Huella > Vibración del teclado (or iOS's Keyboard Feedback > Haptic);
#   a notification / message vibrates per Settings > Sounds & Haptics; an incoming call rings until answered or
#   declined; fingerprint: success / error / long (lockout).
#   vibtest.sh sblog [S]      SapphireHaptics lines (oslog, S seconds, default 30)
#   vibtest.sh rollback       the vibd / tweak before the last install (*.prev); vibtest.sh uninstall
#   Trouble in apps: touch /var/jb/var/sapphire-haptics-noaudiohook (no AudioServices hook), or
#   rm /var/jb/usr/lib/TweakInject/SapphireHaptics.* and restart the app / SpringBoard.
set -e
cd "$(dirname "$0")"
step=${1:-help}
P=${2:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
phone() { ssh $O $P /var/jb/bin/bash -s "$@"; }
HDR='export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin'
case $step in
install) bash install.sh "$P" ;;
uninstall) bash uninstall.sh "$P" ;;
status)
	phone <<EOF
$HDR
pgrep -l vibd || echo "vibd not running"
launchctl print system/com.sapphire.vibd 2>/dev/null | grep -E "state|pid|last exit"
echo "kern.sweet_vibrate = \$(sysctl -n kern.sweet_vibrate 2>&1)"; echo "pattern = \$(sysctl -n kern.sweet_vibrate_pattern 2>&1)"
ls -l /var/jb/usr/lib/TweakInject/SapphireHaptics.* /redwood/bin/vibd
tail -15 /var/tmp/vibd.log 2>/dev/null
EOF
	;;
preset) phone <<<"$HDR; notifyutil -p com.sapphire.vib.${3:-medium}" ;;
all)
	phone <<EOF
$HDR
for p in selection light medium heavy success warning error notify long; do echo \$p; notifyutil -p com.sapphire.vib.\$p; sleep 1.2; done
EOF
	;;
ring) phone <<<"$HDR; notifyutil -p com.sapphire.vib.ring_start; echo 'ringing (vibtest.sh stop)'" ;;
stop) phone <<<"$HDR; notifyutil -p com.sapphire.vib.stop" ;;
sblog)
	phone "${3:-30}" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
oslog --debug > /var/tmp/hap.all 2>&1 &
sleep "$1"; killall oslog 2>/dev/null
sed 's/\x1b\[[0-9;]*m//g' /var/tmp/hap.all | grep -a "SapphireHaptics" | cut -c1-240 | tail -60
EOF
	;;
rollback)
	phone <<EOF
$HDR
for f in /redwood/bin/vibd /var/jb/usr/lib/TweakInject/SapphireHaptics.dylib; do [ -f \$f.prev ] && /var/jb/bin/mv -f \$f.prev \$f && echo "restored \$f"; done
launchctl kickstart -k system/com.sapphire.vibd 2>/dev/null; killall -9 SpringBoard; echo "SpringBoard restarted"
EOF
	;;
*) sed -n '2,19p' "$0" ;;
esac
