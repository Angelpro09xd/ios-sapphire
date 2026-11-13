#!/var/jb/bin/bash
# Tester bug 10. The missing iOS half of the Android app's "IPAs" page.
#
# Today: Huella/dual-boot app (IpaInbox.java) writes *.ipa entries into the inbox after iOS's volume, and
# nothing on iOS ever reads them: userland/update/sapphire-update.sh only takes *.supdate
# ("case "$name" in *.supdate) ;; *) continue"). So the IPA installer "does nothing".
#
# This: run from /redwood/boot.d (or a launchd job) once SpringBoard is up; every *.ipa entry not yet
# handled is read through kern.sweet_inbox (sapphire-inboxcat), its sha256 checked, and installed with
# TrollStore's helper (present in the overlay: /var/jb/Applications/TrollStorePersistenceHelper.app).
# The result goes to /var/root/.sapphire-ipas ("SHA256 installed|failed NAME EPOCH"): the inbox is
# read-only from iOS (0310), so the header's per-entry state (48: 0 pending, 1 installed, 2 failed) can
# not be written here. Android then shows the state from that file instead (it already reads the HFS
# volume: Hfs.java) -> IpaPage.fill() needs a small change: state = file's line for e.sha256, else PENDING.
#
# To check on the phone before shipping: the exact trollstorehelper CLI of the bundled version
# ("install <path>"; some versions want "install <path> force" for unsigned/fakesigned IPAs), and that it
# registers the app with lsd (uicache) by itself.
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin
CAT=/redwood/bin/sapphire-inboxcat
HELPER=/var/jb/Applications/TrollStorePersistenceHelper.app/trollstorehelper
STATE=/var/root/.sapphire-ipas
LOG=/var/root/sapphire-ipa.log
W=/var/tmp/sapphire-ipa
log() { echo "$(date '+%F %T') $*" >> $LOG; echo "sapphire-ipa: $*" > /dev/console 2>/dev/null; }

[ -x $CAT ] && [ -x $HELPER ] || exit 0
# installd/lsd/SpringBoard must be up: wait (at most ~5 min) for SpringBoard and a calm boot
for i in $(seq 1 60); do
	killall -0 SpringBoard 2>/dev/null && [ "$(sysctl -n kern.boottime | sed 's/.*sec = \([0-9]*\).*/\1/')" -lt $(( $(date +%s) - 90 )) ] && break
	sleep 5
done
S=$($CAT start) || exit 0
hdr=$(mktemp)
$CAT "$S" 4096 > "$hdr" 2>/dev/null || { rm -f "$hdr"; exit 0; }
[ "$(head -c 8 "$hdr")" = SPHIPAQ1 ] || { rm -f "$hdr"; exit 0; }
le() { od -An -t u$2 -j $1 -N $2 "$hdr" | tr -d ' '; }
count=$(le 12 4)
touch $STATE
for i in $(seq 0 $(( count - 1 ))); do
	e=$(( 24 + 256 * i ))
	name=$(dd if="$hdr" bs=1 skip=$(( e + 56 )) count=192 2>/dev/null | tr -d '\0')
	case "$name" in *.ipa|*.IPA|*.tipa) ;; *) continue ;; esac
	off=$(le $e 8); len=$(le $(( e + 8 )) 8)
	sha=$(od -An -tx1 -j $(( e + 16 )) -N 32 "$hdr" | tr -d ' \n')
	grep -q "^$sha " $STATE && continue
	rm -rf $W; mkdir -p $W
	f="$W/app.ipa"
	if ! $CAT $(( S + off )) "$len" > "$f"; then
		log "$name: the inbox could not be read"; continue
	fi
	got=$(sha256sum "$f" | cut -d' ' -f1)
	if [ "$got" != "$sha" ]; then
		echo "$sha failed $name $(date +%s)" >> $STATE; log "$name: sha256 mismatch"; continue
	fi
	if $HELPER install "$f" >> $LOG 2>&1 || $HELPER install "$f" force >> $LOG 2>&1; then
		echo "$sha installed $name $(date +%s)" >> $STATE; log "$name: installed"
	else
		echo "$sha failed $name $(date +%s)" >> $STATE; log "$name: trollstorehelper failed ($?)"
	fi
	rm -rf $W
done
rm -f "$hdr"
uicache -a >/dev/null 2>&1 &
exit 0
