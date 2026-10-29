#!/bin/bash
# fptest.sh STEP [PHONE] [ARG]: the fingerprint unlock test plan (v2), run from WSL (phone root@192.168.7.2).
#
# Needs: a kernel with 0261 (kern.sweet_qsee ops 10-27, kern.sweet_fod_*) booted with qsee=1, a passcode set,
# the factory files (and any enrolled finger) in /var/root/fp.
#
# Install / update
#   fptest.sh build          builds everything into ~/fp-out (no phone)
#   fptest.sh update         update.sh: fpd/fpctl, both tweaks, the Huella app; fpd restarted, SpringBoard restarted
#                            (for a phone that already has v1; launchd's cache untouched)
#   fptest.sh install        install.sh: first install (also adds the job to launchd's cache, backup .pre-fpd)
#   fptest.sh app            the Huella app only (install-app.sh: trollstorehelper, else /Applications + uicache)
#   fptest.sh start | stop | status | respring
# Checks (phone unlocked)
#   1. fptest.sh ctl STATUS        "OK ... sensor=1 version=2"; fptest.sh ctl LIST: FINGER 0 ... Huella 1
#   2. Settings: a "Huella dactilar" row under "Face ID / Touch ID y código" opens Huella (killall Preferences first).
#   3. Huella: Probar huella (finger on the red circle: "Reconocida"); Añadir una huella (the fingerprint fills,
#      hints on "Mueve el dedo", Completado, name it); the list shows it; rename; delete it again.
#      Same from the shell: fptest.sh ctl ENROLL_BEGIN Pulgar (Ctrl-C cancels), ctl TEST, ctl DELETE 1.
#   4. Look: change style/colour/size/opacity/animation in Huella, lock: the glyph follows (com.sapphire.fp.prefs).
#   5. Lock screen: the glyph over the sensor; swipe up to the keypad: the glyph sits under the "0" key and the
#      finger still unlocks; a wrong finger: shake + "Huella no reconocida (n/5)"; 5: "Demasiados intentos".
#   6. Screen off: enable "Desbloquear tocando con la pantalla apagada", screen off, finger on the sensor.
#      fpd.log says "press N with the screen off" if the touch panel reports with the display off (else nothing).
#   fptest.sh logs | sblog [S]     fpd.log (follow) / SpringBoard + Settings tweak lines for S seconds (oslog)
#   fptest.sh sim-glyph | sim-shake | sim-unlock | sim-press | sim-wake    the tweak's reactions without the sensor
# Rollback
#   fptest.sh rollback-update      the binaries/tweaks before the last update.sh (*.prev), fpd + SpringBoard restarted
#   fptest.sh uninstall            uninstall.sh (job out of the cache, tweaks, app, binaries removed)
#   Worst case: ssh in, `touch /var/jb/var/sapphire-notweaks; killall -9 SpringBoard` (all tweaks off), or
#   rm /var/jb/usr/lib/TweakInject/SapphireFingerprint*; launchd cache from before: launchd.plist.pre-fpd.
#   /var/root/fp/orig/ keeps the first version of every template file fpd replaced after a study update.
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
update)
	bash update.sh "$P" ;;
app)
	d=$(mktemp -d); bash build.sh "$d" > /dev/null; bash install-app.sh "$d/Huella.ipa" "$P"; rm -rf "$d" ;;
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
/redwood/bin/fpctl STATUS 2>&1; /redwood/bin/fpctl LIST 2>&1
for s in kern.sweet_qsee kern.sweet_fod_press kern.sweet_fod_seq kern.sweet_qsee_arg; do echo "\$s = \$(sysctl -n \$s 2>&1)"; done
sysctl -n kern.sweet_qsee_files 2>/dev/null | head -20
/var/root/mkb 2>/dev/null | head -5
ls -l /var/root/fp /var/jb/usr/lib/TweakInject/SapphireFingerprint* /var/run/fpd.sock
tail -25 /var/root/fp/fpd.log 2>/dev/null; tail -5 /var/tmp/fpd.out 2>/dev/null
EOF
	;;
ctl)
	shift
	# fptest.sh ctl CMD...  (the phone: root@192.168.7.2, or FP_PHONE=user@host)
	P=${FP_PHONE:-root@192.168.7.2}
	ssh -t $O $P /redwood/bin/fpctl "$@" ;;
logs)
	phone <<<"tail -n 40 -f /var/root/fp/fpd.log" ;;
sblog)
	phone "${3:-30}" <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
oslog --debug > /var/tmp/sfp.all 2>&1 &
sleep "$1"; killall oslog 2>/dev/null
sed 's/\x1b\[[0-9;]*m//g' /var/tmp/sfp.all | grep -a "SapphireFingerprint\|com.sapphire.fpd" | cut -c1-240 | tail -80
EOF
	;;
respring)
	phone <<<"$HDR; killall -9 SpringBoard; echo SpringBoard restarted" ;;
rollback-update)
	phone <<EOF
$HDR
T=/var/jb/usr/lib/TweakInject
for f in /redwood/bin/fpd \$T/SapphireFingerprint.dylib \$T/SapphireFingerprintSettings.dylib; do
	if [ -f \$f.prev ]; then /var/jb/bin/mv -f \$f.prev \$f && echo "restored \$f"; fi
done
launchctl kickstart -k system/com.sapphire.fpd 2>/dev/null && echo "fpd restarted"
killall Preferences 2>/dev/null; killall -9 SpringBoard; echo "SpringBoard restarted"
EOF
	;;
sim-glyph)
	phone <<<"$HDR; notifyutil -s com.sapphire.fp.state 1 && notifyutil -p com.sapphire.fp.state && echo 'state 1 posted (fpd running overrides it)'" ;;
sim-shake)
	phone <<<"$HDR; notifyutil -s com.sapphire.fp.fails 2; notifyutil -p com.sapphire.fp.nomatch" ;;
sim-unlock)
	phone <<<"$HDR; notifyutil -p com.sapphire.fp.unlocked" ;;
sim-press)
	phone <<<"$HDR; notifyutil -p com.sapphire.fp.press" ;;
sim-wake)
	phone <<<"$HDR; notifyutil -p com.sapphire.fp.wake" ;;
*)
	sed -n '2,36p' "$0" ;;
esac
