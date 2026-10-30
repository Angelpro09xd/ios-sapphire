# xfer.sh: sourced by install.sh / uninstall.sh. launchd's cache is ~800 KB and a plain scp of it has stalled the
# USB link (NOTES: big transfers stall usbipd); these move it gzipped (~100 KB), with a timeout and three tries.
#   pull REMOTE LOCAL     push LOCAL REMOTE      (need $O and $P)
pull() {
	local t
	for t in 1 2 3; do
		if timeout 60 ssh $O $P "/var/jb/usr/bin/gzip -c '$1'" > "$2.gz" && gunzip -f "$2.gz"; then return 0; fi
		echo "pull $1: try $t failed"; sleep 3
	done
	return 1
}
push() {
	local t
	gzip -c "$1" > "$1.gz"
	for t in 1 2 3; do
		if timeout 60 ssh $O $P "/var/jb/usr/bin/gzip -dc > '$2'" < "$1.gz"; then rm -f "$1.gz"; return 0; fi
		echo "push $2: try $t failed"; sleep 3
	done
	return 1
}
