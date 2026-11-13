#!/usr/bin/env bash
# sapphire-install.sh: Sapphire's installer for Linux (Arch, Debian/Ubuntu and friends): the same steps as
# SapphireInstaller.exe. The phone does the work in OrangeFox with sapphire-recovery.zip; this script feeds it
# the release's files (next to this script) and the user's IPSW over adb.
#
#   ./sapphire-install.sh /path/to/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw
#
# Needs: adb, fastboot, python3 (Arch: pacman -S android-tools python; Debian/Ubuntu: apt install adb fastboot
# python3). Spanish if $LANG is Spanish, English otherwise.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SH=/tmp/sapphire/sapphire.sh
PORT=5550
RECOVERY_ZIP=$HERE/sapphire-recovery.zip
OVERLAY=$HERE/sapphire-overlay.tar.gz
KERNEL=$HERE/sapphire-kernel.img
APK=$HERE/SapphireDualBoot.apk
MIN_IOS=$((24 << 30))
MIN_ANDROID=$((16 << 30))

case "${LANG:-}${LC_ALL:-}${LC_MESSAGES:-}" in *es_*|*es.*|es) L=es ;; *) L=en ;; esac
t() { if [ "$L" = es ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

B=$(tput bold 2>/dev/null || true); N=$(tput sgr0 2>/dev/null || true)
R=$(tput setaf 1 2>/dev/null || true); G=$(tput setaf 2 2>/dev/null || true); Y=$(tput setaf 3 2>/dev/null || true)
say() { printf '%s\n' "$*"; }
step() { printf '\n%s== %s%s\n' "$B" "$*" "$N"; }
warn() { printf '%s%s%s\n' "$Y" "$*" "$N"; }
die() { printf '\n%s✗ %s%s\n' "$R" "$*" "$N" >&2; exit 1; }
ask() { local a; printf '%s ' "$1" >&2; read -r a; printf '%s' "$a"; }

# ---------------------------------------------------------------- checks

# --update: update mode (iOS, its apps and data kept; no IPSW): the newest *.supdate next to the script
UPDATE=0
if [ "${1:-}" = --update ]; then
	UPDATE=1
	UPD=$(ls -1 "$HERE"/*.supdate 2>/dev/null | sort | tail -1)
	[ -n "$UPD" ] || die "$(t "No hay ninguna actualización (.supdate) junto al script." "There is no update (.supdate) next to the script.")"
	NEED="$RECOVERY_ZIP $KERNEL"
else
	[ $# -ge 1 ] || die "$(t "Uso: $0 /ruta/a/la.ipsw   (o $0 --update)" "Usage: $0 /path/to/the.ipsw   (or $0 --update)")"
	IPSW=$1
	[ -f "$IPSW" ] || die "$(t "No existe: $IPSW" "Not found: $IPSW")"
	NEED="$RECOVERY_ZIP $OVERLAY $KERNEL"
fi
for f in $NEED; do
	[ -f "$f" ] || die "$(t "Falta $(basename "$f") junto al script: descomprime todo el ZIP en una carpeta." \
	    "$(basename "$f") is missing next to the script: unzip the whole ZIP into one folder.")"
done
for c in adb fastboot python3; do
	command -v $c >/dev/null 2>&1 || die "$(t "Falta '$c'. Arch: sudo pacman -S android-tools python   Debian/Ubuntu: sudo apt install adb fastboot python3" \
	    "'$c' is missing. Arch: sudo pacman -S android-tools python   Debian/Ubuntu: sudo apt install adb fastboot python3")"
done

cat <<EOF

${B}Sapphire v0.1-test${N} — $(t "iOS 16.7.16 junto a Android en el Redmi Note 13 4G (sapphire)" "iOS 16.7.16 next to Android on the Redmi Note 13 4G (sapphire)")
$(t "Sapphire no está afiliado a Apple ni a Xiaomi." "Sapphire is not affiliated with Apple or Xiaomi.")
EOF

# ---------------------------------------------------------------- the IPSW (python: zip + BuildManifest.plist)

if [ $UPDATE = 0 ]; then
step "$(t "Leyendo la IPSW" "Reading the IPSW")"
IPSWINFO=$(python3 - "$IPSW" <<'PY'
import plistlib, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
try:
    bm = plistlib.loads(z.read("BuildManifest.plist"))
except KeyError:
    print("ERR noipsw"); sys.exit()
ver, types = bm.get("ProductVersion", "?"), bm.get("SupportedProductTypes", [])
if "iPhone10,3" not in types:
    print("ERR device " + ",".join(types)); sys.exit()
if not ver.startswith("16."):
    print("ERR version " + ver); sys.exit()
for bi in bm.get("BuildIdentities", []):
    info, man = bi.get("Info", {}), bi.get("Manifest", {})
    if info.get("DeviceClass", "").lower() == "d22ap" and "Erase" in info.get("Variant", ""):
        p = [man.get(k, {}).get("Info", {}).get("Path") for k in ("OS", "Cryptex1,SystemOS", "Cryptex1,AppOS")]
        if all(p):
            sizes = [z.getinfo(x).file_size for x in p]
            print("OK %s %s %s %d %s %d %s %d" % (ver, bm.get("ProductBuildVersion", "?"), p[0], sizes[0], p[1], sizes[1], p[2], sizes[2]))
            sys.exit()
print("ERR identity")
PY
)
set -- $IPSWINFO
case "$1" in
OK) VERSION=$2; BUILD=$3; ROOT_DMG=$4; ROOT_SIZE=$5; SYS_DMG=$6; SYS_SIZE=$7; APP_DMG=$8; APP_SIZE=$9 ;;
*) die "$(t "Esta IPSW no vale ($IPSWINFO). Hace falta iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw." \
    "This IPSW does not work ($IPSWINFO). You need iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw.")" ;;
esac
say "iOS $VERSION ($BUILD): $ROOT_DMG, $SYS_DMG, $APP_DMG"
fi

# ---------------------------------------------------------------- the phone

state() {
	local s
	s=$(adb devices 2>/dev/null | awk 'NR>1 && $2 != "" {print $2; exit}')
	if [ -n "$s" ]; then echo "$s"; return; fi
	fastboot devices 2>/dev/null | grep -q fastboot && echo fastboot
}

wait_for() {   # STATE SECONDS
	local i
	for i in $(seq 1 "$2"); do
		[ "$(state)" = "$1" ] && return 0
		sleep 1
	done
	return 1
}

step "$(t "Teléfono" "Phone")"
say "$(t "Conecta el teléfono por USB (en Android con la depuración USB, en OrangeFox o en fastboot)." \
    "Connect the phone over USB (in Android with USB debugging, in OrangeFox or in fastboot).")"
for i in $(seq 1 120); do
	S=$(state)
	case "$S" in
	recovery|device|fastboot) break ;;
	unauthorized) [ $((i % 10)) = 1 ] && warn "$(t "Acepta el aviso de depuración USB en el teléfono." "Accept the USB debugging prompt on the phone.")" ;;
	esac
	sleep 2
done
case "$S" in
device)
	say "$(t "Teléfono en Android: reiniciando a OrangeFox." "Phone in Android: restarting into OrangeFox.")"
	adb reboot recovery ;;
fastboot)
	say "$(t "Teléfono en fastboot: arrancando OrangeFox." "Phone in fastboot: starting OrangeFox.")"
	fastboot reboot recovery ;;
recovery) ;;
*) die "$(t "No encuentro el teléfono." "The phone was not found.")" ;;
esac
wait_for recovery 120 || die "$(t "OrangeFox no apareció. Si pide desbloquear (patrón/PIN), hazlo en el teléfono y vuelve a ejecutar." \
    "OrangeFox did not show up. If it asks to be unlocked (pattern/PIN), do it on the phone and run again.")"

load_zip() {
	# OrangeFox restarts its adbd while it starts: loaded once adb answers 3 times in a row, 10 s later,
	# and tried again up to 3 times
	local attempt ok i
	for attempt in 1 2 3; do
		say "$(t "Esperando a que OrangeFox termine de arrancar" "Waiting for OrangeFox to finish starting")"
		ok=0
		for i in $(seq 1 120); do
			if [ "$(adb shell echo ready 2>/dev/null | tr -d '\r')" = ready ]; then ok=$((ok + 1)); else ok=0; fi
			[ $ok -ge 3 ] && break
			sleep 1
		done
		sleep 10
		load_zip_once && return 0
		warn "$(t "OrangeFox aún no estaba listo: reintentando…" "OrangeFox was not ready yet: trying again…")"
		sleep 5
	done
	die "$(t "El ZIP no arrancó en OrangeFox." "The ZIP did not start in OrangeFox.")"
}

load_zip_once() {
	say "$(t "Cargando Sapphire en OrangeFox" "Loading Sapphire into OrangeFox")"
	adb shell rm -rf /tmp/sapphire /tmp/sapphire.zip >/dev/null 2>&1
	adb push "$RECOVERY_ZIP" /tmp/sapphire.zip >/dev/null || return 1
	adb shell twrp install /tmp/sapphire.zip >/dev/null 2>&1
	INFO=$(adb shell "$SH info" 2>/dev/null | tr -d '\r')
	echo "$INFO" | grep -q '^device=' || return 1
	adb shell "$SH lang $L" >/dev/null
	adb shell "$SH screen-on" >/dev/null
}
val() { echo "$INFO" | sed -n "s/^$1=//p" | head -1; }
load_zip

ANDROID_SLOT=$(val android_slot)
IOS_BYTES=$(val iosroot_bytes); IOS_BYTES=${IOS_BYTES:-0}
UD_BYTES=$(val userdata_bytes); UD_BYTES=${UD_BYTES:-0}
TOTAL=$((IOS_BYTES + UD_BYTES))
gb() { python3 -c "print('%.1f GB' % ($1 / 2**30))"; }
say "$(t "Dispositivo" "Device"): $(val device)   ·   Android: slot $ANDROID_SLOT"
if [ "$IOS_BYTES" -gt 0 ]; then
	say "$(t "iOS instalado ahora" "iOS installed now"): $(gb $IOS_BYTES) $(t "(se borrará y se instalará de nuevo)" "(it will be erased and installed again)")"
else
	say "$(t "iOS no está instalado: hay que hacerle sitio." "iOS is not installed: room has to be made for it.")"
fi
say "$(t "Espacio para repartir" "Space to split"): $(gb $TOTAL)"

if [ $UPDATE = 1 ]; then
	[ "$IOS_BYTES" -gt 0 ] || die "$(t "iOS no está instalado en este teléfono: instálalo primero (sin --update)." "iOS is not installed on this phone: install it first (without --update).")"
	step "$(t "Confirmar" "Confirm")"
	say "•  $(t "Actualización" "Update"): $(basename "$UPD")"
	say "•  $(t "Kernel de la release" "The release's kernel") ($(basename "$KERNEL"))"
	say "•  $(t "iOS, tus apps, tus datos y Android no se tocan." "iOS, your apps, your data and Android are not touched.")"
	B1=$(ask "$(t "¿Arrancar iOS al terminar (así se aplica la actualización)? [S/n]:" "Boot iOS when it is done (that is when the update is applied)? [Y/n]:")")
	case "$B1" in [nN]*) BOOT_IOS=0 ;; *) BOOT_IOS=1 ;; esac
else
# ---------------------------------------------------------------- storage

step "$(t "Espacio" "Storage")"
if [ "$IOS_BYTES" -gt 0 ]; then DEF=$((IOS_BYTES >> 30)); else DEF=40; fi
say "$(t "iOS necesita al menos 24 GB y Android al menos 16 GB." "iOS needs at least 24 GB and Android at least 16 GB.")"
while :; do
	A=$(ask "$(t "GB para iOS [$DEF]:" "GB for iOS [$DEF]:")")
	A=${A:-$DEF}
	case "$A" in *[!0-9]*|'') warn "$(t "Escribe un número." "Type a number.")"; continue ;; esac
	WANT=$((A << 30))
	[ "$IOS_BYTES" -gt 0 ] && [ "$A" = "$DEF" ] && WANT=$IOS_BYTES
	if [ $WANT -lt $MIN_IOS ] || [ $((TOTAL - WANT)) -lt $MIN_ANDROID ]; then
		warn "$(t "Fuera de límites." "Out of range.")"; continue
	fi
	break
done
REPART=0
if [ "$IOS_BYTES" -eq 0 ] || [ "$WANT" -ne "$IOS_BYTES" ]; then
	REPART=1
	printf '\n%s%s%s\n' "$R$B" "$(t "⚠  ESTO BORRA TODOS LOS DATOS DE ANDROID (apps, fotos, todo). Haz una copia antes." \
	    "⚠  THIS ERASES ALL OF ANDROID'S DATA (apps, photos, everything). Back up first.")" "$N"
else
	say "$G$(t "✓  El reparto no cambia: los datos de Android se conservan." "✓  The split does not change: Android's data is kept.")$N"
fi

# ---------------------------------------------------------------- confirm

step "$(t "Confirmar" "Confirm")"
say "•  iOS $VERSION ($BUILD) $(t "se instala de cero en" "is installed from scratch in") $(gb $WANT)"
say "•  $(t "Kernel de la release" "The release's kernel") ($(basename "$KERNEL"))"
B1=$(ask "$(t "¿Al terminar arrancar iOS directamente? [s/N]:" "Boot iOS right away when it is done? [y/N]:")")
case "$B1" in [sSyY]*) BOOT_IOS=1 ;; *) BOOT_IOS=0 ;; esac
WORD=$(t BORRAR ERASE)
C=$(ask "$(t "Escribe $WORD para empezar (borra iOS$( [ $REPART = 1 ] && echo ' y ANDROID')):" \
    "Type $WORD to start (erases iOS$( [ $REPART = 1 ] && echo ' and ANDROID')):")")
[ "$C" = "$WORD" ] || die "$(t "Cancelado." "Cancelled.")"
fi

# ---------------------------------------------------------------- install

phone() {   # ARGS... : sapphire.sh over adb shell; its STEP/ERROR lines shown; fails on ERROR or a bad exit code
	local okrc=${OKRC:-0} out rc
	out=$(adb shell "$SH $*; echo EXIT=\$?" 2>&1 | tr -d '\r')
	rc=$(echo "$out" | sed -n 's/^EXIT=//p' | tail -1)
	echo "$out" | grep -v -E '^(EXIT=|PROGRESS )' | sed -e 's/^STEP /  /' -e 's/^/  /'
	if echo "$out" | grep -q '^ERROR '; then die "$(echo "$out" | sed -n 's/^ERROR //p' | head -1)"; fi
	[ "$rc" = 0 ] || [ "$rc" = "$okrc" ] || die "$(t "El teléfono devolvió el error $rc ($1)" "The phone returned error $rc ($1)")"
}

send() {   # LABEL SIZE SOURCE [ZIPENTRY] -- SIZE bytes over TCP to sapphire-recv, CRC32-checked
	local label=$1 size=$2 src=$3 entry=${4:-} log pid crc got
	say "  $(t "Enviando" "Sending") $label ($(gb "$size"))"
	adb forward tcp:$PORT tcp:$PORT >/dev/null || die "adb forward"
	log=$(mktemp)
	adb shell "$SH $label" > "$log" 2>&1 &
	pid=$!
	for i in $(seq 1 90); do grep -q '^LISTEN' "$log" && break; grep -q '^ERROR' "$log" && break; sleep 1; done
	grep -q '^LISTEN' "$log" || { cat "$log"; die "$(t "El teléfono no se puso a escuchar" "The phone did not start listening")"; }
	crc=$(python3 - "$src" "$entry" "$size" "$PORT" <<'PY'
import socket, sys, zipfile, zlib
src, entry, size, port = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = zipfile.ZipFile(src).open(entry) if entry else open(src, "rb")
s = socket.create_connection(("127.0.0.1", port))
crc, done = 0, 0
while done < size:
    b = f.read(min(1 << 20, size - done))
    if not b:
        sys.exit("source ended early")
    s.sendall(b)
    crc = zlib.crc32(b, crc)
    done += len(b)
    if done % (64 << 20) < len(b) or done == size:
        sys.stderr.write("\r    %3d %%" % (done * 100 // size))
sys.stderr.write("\n")
# no half-close: sapphire-recv knows the size; it closes once it has written everything
s.settimeout(900)
while s.recv(4096):
    pass
s.close()
print("%08x" % (crc & 0xffffffff))
PY
) || die "$(t "Falló el envío de $label" "Sending $label failed")"
	wait $pid
	got=$(tr -d '\r' < "$log" | sed -n 's/^RECV //p' | tail -1)
	grep '^ERROR' "$log" && die "$(tr -d '\r' < "$log" | sed -n 's/^ERROR //p' | head -1)"
	[ "$got" = "$size $crc" ] || die "$(t "El teléfono no recibió $label bien ($got / $size $crc)" "The phone did not receive $label correctly ($got / $size $crc)")"
	rm -f "$log"
	adb forward --remove tcp:$PORT >/dev/null 2>&1
}

step "$(t "Preparar el teléfono" "Prepare the phone")"
phone check
adb shell "$SH screen '$(t "Instalando: no desconectes el cable" "Installing: do not unplug the cable")'" >/dev/null

if [ $UPDATE = 1 ]; then
	step "$(t "Enviar la actualización" "Send the update")"
	USIZE=$(stat -c %s "$UPD")
	send "update $(basename "$UPD") $USIZE" "$USIZE" "$UPD"
else

step "$(t "Repartir el espacio" "Split the storage")"
SERIAL=$(adb get-serialno 2>/dev/null | tr -d '\r')
PENDING=${XDG_CACHE_HOME:-$HOME/.cache}/sapphire/format-pending-${SERIAL:-phone}
if [ $REPART = 1 ]; then
	OKRC=3 phone part "$WANT"
	mkdir -p "$(dirname "$PENDING")" && date > "$PENDING"
	say "$(t "Reiniciando OrangeFox para leer la nueva tabla" "Restarting OrangeFox to read the new partition table")"
	adb reboot recovery
	sleep 5
	wait_for recovery 180 || die "$(t "OrangeFox no volvió. La tabla nueva ya está escrita: vuelve a ejecutar el script con el teléfono en OrangeFox." \
	    "OrangeFox did not come back. The new table is written: run the script again with the phone in OrangeFox.")"
	load_zip
	phone format-data
	rm -f "$PENDING"
elif [ -f "$PENDING" ]; then
	say "  $(t "La instalación anterior repartió el espacio pero no llegó a formatear Android: se formatea ahora." \
	    "The previous install split the storage but did not get to format Android: formatting it now.")"
	phone format-data
	rm -f "$PENDING"
else
	say "  $(t "El reparto no cambia: los datos de Android se conservan." "The split does not change: Android's data is kept.")"
fi

step "$(t "Enviar iOS al teléfono" "Send iOS to the phone")"
phone format
send "recv root $ROOT_SIZE" "$ROOT_SIZE" "$IPSW" "$ROOT_DMG"
send "recv sysos $SYS_SIZE" "$SYS_SIZE" "$IPSW" "$SYS_DMG"
send "recv appos $APP_SIZE" "$APP_SIZE" "$IPSW" "$APP_DMG"
OVSIZE=$(stat -c %s "$OVERLAY")
send "recv overlay $OVSIZE" "$OVSIZE" "$OVERLAY"

step "$(t "Construir el sistema de iOS (unos 7 minutos)" "Build iOS's system (about 7 minutes)")"
phone rtc "$(date -u +%s)"
phone build
# 0.1.8: the release's update too (what changed after the overlay was made), applied by iOS at its first boot
UPD=$(ls -1 "$HERE"/*.supdate 2>/dev/null | sort | tail -1)
if [ -n "$UPD" ]; then
	USIZE=$(stat -c %s "$UPD")
	say "  $(t "Enviando la actualización de la release" "Sending the release's update") ($(basename "$UPD"))"
	send "update $(basename "$UPD") $USIZE" "$USIZE" "$UPD"
fi

fi

step "$(t "Instalar el kernel" "Install the kernel")"
KSIZE=$(stat -c %s "$KERNEL")
send "kernel $KSIZE" "$KSIZE" "$KERNEL"

step "$(t "Terminar" "Finish")"
phone finish
if [ $BOOT_IOS = 1 ]; then
	IOS_SLOT=$([ "$ANDROID_SLOT" = a ] && echo b || echo a)
	say "$(t "Reiniciando al bootloader para arrancar iOS (slot $IOS_SLOT)" "Restarting into the bootloader to start iOS (slot $IOS_SLOT)")"
	adb reboot bootloader
	wait_for fastboot 120 || die "$(t "El teléfono no entró en fastboot." "The phone did not enter fastboot.")"
	fastboot set_active "$IOS_SLOT" || die "fastboot set_active $IOS_SLOT"
	fastboot reboot
else
	say "$(t "Reiniciando a Android" "Restarting into Android")"
	adb reboot
	if [ -f "$APK" ]; then
		say "$(t "Esperando a Android para instalar Sapphire Dual Boot (activa la depuración USB si la pide)..." \
		    "Waiting for Android to install Sapphire Dual Boot (allow USB debugging if asked)...")"
		if wait_for device 300; then
			adb wait-for-device shell 'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done'
			adb install -r "$APK" >/dev/null && say "$(t "App instalada. Ábrela para cambiar a iOS." "App installed. Open it to switch to iOS.")" \
			    || warn "$(t "No se pudo instalar la app: instálala luego desde SapphireDualBoot.apk." "The app could not be installed: install SapphireDualBoot.apk later.")"
		else
			warn "$(t "Android no apareció por adb: instala SapphireDualBoot.apk luego." "Android did not show up over adb: install SapphireDualBoot.apk later.")"
		fi
	fi
fi
if [ $UPDATE = 1 ]; then
	printf '\n%s%s%s\n' "$G$B" "$(t "¡Listo! iOS aplica la actualización al arrancar." "Done! iOS applies the update as it starts.")" "$N"
	exit 0
fi
printf '\n%s%s%s\n' "$G$B" "$(t "¡Listo! El primer arranque de iOS puede tardar 15 minutos: no lo reinicies." \
    "Done! iOS's first boot can take 15 minutes: do not restart it.")" "$N"
