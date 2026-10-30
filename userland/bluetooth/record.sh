#!/bin/bash
# record.sh [SECONDS] [PHONE]: the recording run. Installs the Bluetooth jobs in fake-controller mode (install.sh --fake:
# btshim daemon -F -v -w 0, no hardware), lets bluetoothd initialise against the fake BCM4357B1 for SECONDS (120),
# optionally while you open Settings > Bluetooth and toggle it, then pulls /var/tmp/btshim.log (+ bluetoothd's and
# BlueTool's stdout) to ~/ios-build/bt/logs/record-STAMP.* and prints the opcodes bluetoothd sent (count, opcode,
# how btshim answered). The fake mode stays installed; ./install.sh (no --fake) switches to the real controller.
set -e
cd "$(dirname "$0")"
secs=${1:-120}
P=${2:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
ssh $O $P "/var/jb/bin/rm -f /var/tmp/btshim.log /var/tmp/btshim-bluetool.out"
bash install.sh --fake "$P"
echo "recording ${secs} s (open Settings > Bluetooth and toggle it now if you want that in the log) ..."
sleep "$secs"
d=$HOME/ios-build/bt/logs
mkdir -p "$d"
stamp=$(date +%Y%m%d-%H%M%S)
scp -q $O $P:/var/tmp/btshim.log "$d/record-$stamp.log"
scp -q $O $P:/var/tmp/btshim-bluetool.out "$d/record-$stamp.bluetool.out" 2>/dev/null || true
ssh $O $P "/var/jb/bin/bash -c 'pgrep -l -f \"btshim|bluetoothd\"; launchctl print system/com.apple.bluetoothd 2>/dev/null | grep -E \"state|runs|last exit\"'" \
	> "$d/record-$stamp.state" 2>&1 || true
echo "== $d/record-$stamp.log"
cat "$d/record-$stamp.state"
echo "== commands from bluetoothd (count opcode first-param-bytes):"
# hexdump lines: "... H>   01 lo hi len p0 p1 ..."
grep -E '\] H>  +01 ' "$d/record-$stamp.log" | sed -E 's/.*H> +//' |
	awk '{ op = $3 $2; printf "0x%s %s\n", op, $5 " " $6 " " $7 }' | sort | uniq -c | sort -k2 | head -150
echo "== answers:"
grep -o -E '(fake|vsc): Command (Complete|Status) for 0x[0-9a-f]+[^)]*\)?' "$d/record-$stamp.log" | sort | uniq -c | sort -rn | head -80
echo "== BlueTool requests:"
grep -E 'bluetool' "$d/record-$stamp.log" | sed -E 's/^[0-9.]+ \[[0-9]+\] //' | sort | uniq -c | head -20
