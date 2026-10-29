#!/bin/bash
# mkfiles.sh OUT.tar.gz: the files the ADSP's sensors PD reads, laid out by the paths it asks for, for sensload.
#   vendor/etc/sensors/**                 from the Lunaris vendor image (~/ios-build/lunaris/fs/vendor/etc/sensors)
#   mnt/vendor/persist/sensors/**         a READ-ONLY copy out of the persist backup image (debugfs rdump; the
#                                         image is opened read-only and nothing is ever written back anywhere)
#   sys/devices/soc0/*                    what sns_reg_config names (soc_id etc.): values for khaje/sapphire, best guesses
set -e
OUT=${1:?usage: mkfiles.sh OUT.tar.gz}
VENDOR=${VENDOR:-$HOME/ios-build/lunaris/fs/vendor/etc/sensors}
PERSIST=${PERSIST:-"/mnt/c/Users/angel/Desktop/ios port/backup-persist/persist-sda2-2026-10-29.img"}
S=$(mktemp -d)
trap 'rm -rf -- "$S"' EXIT
mkdir -p "$S/vendor/etc" "$S/mnt/vendor/persist" "$S/sys/devices/soc0"
cp -r "$VENDOR" "$S/vendor/etc/sensors"
debugfs -R "rdump /sensors $S/mnt/vendor/persist" "$PERSIST" 2>/dev/null
[ -d "$S/mnt/vendor/persist/sensors/registry" ] || { echo "no sensors/registry in $PERSIST"; exit 1; }
printf 'IDP\n'     > "$S/sys/devices/soc0/hw_platform"
printf 'Unknown\n' > "$S/sys/devices/soc0/platform_subtype"
printf '2\n'       > "$S/sys/devices/soc0/platform_subtype_id"
printf '65536\n'   > "$S/sys/devices/soc0/platform_version"
printf '518\n'     > "$S/sys/devices/soc0/soc_id"
printf '1.0\n'     > "$S/sys/devices/soc0/revision"
tar -C "$S" -czf "$OUT" vendor mnt sys
echo "$OUT: $(tar -tzf "$OUT" | grep -vc '/$') files"
