#!/var/jb/bin/bash
# swipetest.sh ROUNDS (runs on the phone): wakes and unlocks if the display is off, goes home, then
# ROUNDS x (page left, page right, Control Center open, close), with hidtouch (xnu 0237). For
# frame rates: compare the backboardd "redwood-gl: frame" lines before and after (every 20th frame).
H=/var/root/hidtouch
N=${1:-5}
D=/var/jb/sbin/dmesg
if $D | grep -a -e 'SWEET FB: display on$' -e 'SWEET FB: display off$' | tail -1 | grep -q 'display off$'; then
	$H press 0; sleep 2
	$H swipe 0.5 0.995 0.5 0.3 250; sleep 2
fi
$H swipe 0.5 0.995 0.5 0.3 250; sleep 1.5      # (home from an app or the lock screen)
echo START
for i in $(seq $N); do
	$H swipe 0.85 0.5 0.15 0.5 250; sleep 0.8
	$H swipe 0.15 0.5 0.85 0.5 250; sleep 0.8
	$H swipe 0.92 0.003 0.92 0.65 300; sleep 1.0
	$H swipe 0.5 0.995 0.5 0.4 250; sleep 1.0
done
echo END
