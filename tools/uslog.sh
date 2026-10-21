#!/bin/bash
# Captura el log del kernel de iOS (sweetusb=2) sin depender de que nadie
# llegue a tiempo: espera a que aparezca la interfaz USB, le pone la IP del
# host y guarda todo lo que llegue por UDP 5140 en un fichero con fecha.
# Se queda dando vueltas, asi que cada arranque del telefono deja el suyo.
#   sudo setsid nohup bash uslog.sh >> ~/ios-build/logs/uslog.out 2>&1 &
set -u
D=${D:-/home/angel/ios-build/logs}
mkdir -p "$D"

while :; do
	IF=""
	while [ -z "$IF" ]; do
		IF=$(ls /sys/class/net | grep '^enx' | head -1)
		[ -z "$IF" ] && sleep 1
	done
	echo "$(date +%H:%M:%S) $IF arriba"
	ip addr add 192.168.7.1/24 dev "$IF" 2>/dev/null
	ip link set "$IF" up 2>/dev/null
	out="$D/boot-$(date +%Y%m%d-%H%M%S).txt"
	python3 -I /home/angel/ios-build/uslog.py "$out" "$IF"
	# la interfaz desaparecio: el telefono se reinicio o se desengancho
	sleep 2
done
