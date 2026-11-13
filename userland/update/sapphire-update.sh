#!/var/jb/bin/bash
# sapphire-update: applies Sapphire updates left in the inbox (by the Windows/Linux installer's update mode;
# later, downloaded from GitHub). Run at every boot by sapphire-firstboot.sh, before anything else of ours.
#
# An update is an inbox entry named *.supdate: a .tar.gz with update.sh at its top (and whatever files it
# brings). It is unpacked to /var/tmp/sapphire-update and run as `bash update.sh DIR` as root. Its sha256
# (from the inbox header, checked against the bytes) goes into /var/root/.sapphire-updates once update.sh
# exits 0, so each update runs once; a failed one is tried again at the next boot (at most 3 times).
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin
CAT=/redwood/bin/sapphire-inboxcat
DONE=/var/root/.sapphire-updates
TRIES=/var/root/.sapphire-update-tries
LOG=/var/root/sapphire-update.log
W=/var/tmp/sapphire-update
log() { echo "$(date '+%F %T') $*" >> $LOG; echo "sapphire-update: $*" > /dev/console 2>/dev/null; }

[ -x $CAT ] || exit 0
S=$($CAT start) || exit 0
hdr=$(mktemp)
$CAT "$S" 4096 > "$hdr" 2>/dev/null || { rm -f "$hdr"; exit 0; }
[ "$(head -c 8 "$hdr")" = SPHIPAQ1 ] || { rm -f "$hdr"; exit 0; }
le() { od -An -t u$2 -j $1 -N $2 "$hdr" | tr -d ' '; }   # OFF SIZE (4 or 8), little-endian
count=$(le 12 4)
touch $DONE $TRIES
for i in $(seq 0 $(( count - 1 ))); do
	e=$(( 24 + 256 * i ))
	name=$(dd if="$hdr" bs=1 skip=$(( e + 56 )) count=192 2>/dev/null | tr -d '\0')
	case "$name" in *.supdate) ;; *) continue ;; esac
	off=$(le $e 8)
	len=$(le $(( e + 8 )) 8)
	sha=$(od -An -tx1 -j $(( e + 16 )) -N 32 "$hdr" | tr -d ' \n')
	grep -q "^$sha" $DONE && continue
	tries=$(grep -c "^$sha" $TRIES)
	if [ "$tries" -ge 3 ]; then
		continue
	fi
	echo "$sha $(date +%s)" >> $TRIES
	log "update $name ($len bytes, $sha): reading"
	rm -rf $W
	mkdir -p $W
	if ! $CAT $(( S + off )) "$len" > $W.tgz; then
		log "update $name: the inbox could not be read"
		continue
	fi
	got=$(shasum -a 256 $W.tgz 2>/dev/null | cut -d' ' -f1)
	[ -n "$got" ] || got=$(sha256sum $W.tgz | cut -d' ' -f1)
	if [ "$got" != "$sha" ]; then
		log "update $name: sha256 mismatch ($got)"
		continue
	fi
	if ! tar -xzpf $W.tgz -C $W 2>>$LOG || [ ! -f $W/update.sh ]; then
		log "update $name: not an update (no update.sh)"
		continue
	fi
	if bash $W/update.sh $W >> $LOG 2>&1; then
		echo "$sha $name $(date +%s)" >> $DONE
		log "update $name: applied"
	else
		log "update $name: update.sh failed ($?)"
	fi
	rm -rf $W $W.tgz
done
rm -f "$hdr"
exit 0
