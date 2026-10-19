#!/bin/bash
# Build the sapphire XNU boot image from a CI kernel:
#
#   tools/sapphire-boot.sh [--readonly] [--console] KERNEL [OUT.img] [extra boot-args]
#
# --readonly drops ufsrw=1/rootrw=1: xnu cannot write the UFS at all (no
# root writes, no persistent log), for test boots with Android still on
# userdata.
# --console keeps the kernel text drawn over the GUI (sweetfb_noconsole=1 is
# left out); without it the log is still in the ring at the end of userdata
# (tools/sweet-log-pull.sh from recovery).
# sweettouch_rx/ry: sapphire raw touch range for patch 0127 (X 0..2159,
# Y 0..2399, two fraction bits); kernels without 0127 ignore them.
# KERNEL is kernel.development.sweet from the xnu workflow artifact. The ADT
# takes /product from research/ipsw-x/d22ap.adt (extracted from the IPSW,
# not in git). The shim fits the RAM to the DDR bank ABL reports.
set -euo pipefail
cd "$(dirname "$0")/.."

rw="ufsrw=1 rootrw=1"
con="sweetfb_noconsole=1"
while [[ ${1:-} == --* ]]; do
	case $1 in
	--readonly) rw="" ;;
	--console) con="" ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
kernel=$(realpath "$1")
out=${2:-shim/out/sapphire-boot.img}
extra=${3:-}

[[ -f research/ipsw-x/d22ap.adt ]] || { echo "missing research/ipsw-x/d22ap.adt" >&2; exit 1; }
mkdir -p shim/out
python3 tools/mkadt.py shim/out/sapphire.adt --highmem
python3 tools/mkpayload.py "$kernel" shim/out/sapphire.adt shim/out/payload.bin \
	--cmdline "debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin $rw launchd_unsecure_cache=1 wdt=-1 $con sweetfb_hz=30 sweettouch_rx=8640 sweettouch_ry=9600 $extra"
shim/build.sh ../shim/out/payload.bin
[[ $out == shim/out/shim-boot.img ]] || cp shim/out/shim-boot.img "$out"
sha256sum "$out"
