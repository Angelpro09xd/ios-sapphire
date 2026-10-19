#!/bin/bash
# Build the sapphire XNU boot image from a CI kernel:
#
#   tools/sapphire-boot.sh [--readonly] KERNEL [OUT.img] [extra boot-args]
#
# --readonly drops ufsrw=1/rootrw=1: xnu cannot write the UFS at all (no
# root writes, no persistent log), for test boots with Android still on
# userdata.
# KERNEL is kernel.development.sweet from the xnu workflow artifact. The ADT
# takes /product from research/ipsw-x/d22ap.adt (extracted from the IPSW,
# not in git). The shim fits the RAM to the DDR bank ABL reports.
set -euo pipefail
cd "$(dirname "$0")/.."

rw="ufsrw=1 rootrw=1"
if [[ ${1:-} == --readonly ]]; then
	rw=""
	shift
fi
kernel=$(realpath "$1")
out=${2:-shim/out/sapphire-boot.img}
extra=${3:-}

[[ -f research/ipsw-x/d22ap.adt ]] || { echo "missing research/ipsw-x/d22ap.adt" >&2; exit 1; }
mkdir -p shim/out
python3 tools/mkadt.py shim/out/sapphire.adt --highmem
python3 tools/mkpayload.py "$kernel" shim/out/sapphire.adt shim/out/payload.bin \
	--cmdline "debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin $rw launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 $extra"
shim/build.sh ../shim/out/payload.bin
[[ $out == shim/out/shim-boot.img ]] || cp shim/out/shim-boot.img "$out"
sha256sum "$out"
