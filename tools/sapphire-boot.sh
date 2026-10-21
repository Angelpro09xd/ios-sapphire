#!/bin/bash
# Build the sapphire XNU boot image from a CI kernel:
#
#   tools/sapphire-boot.sh [--readonly] [--console] [--gpu-fw DIR] KERNEL [OUT.img] [extra boot-args]
#
# --readonly drops ufsrw=1/rootrw=1: xnu cannot write the UFS at all (no
# root writes, no persistent log), for test boots with Android still on
# userdata.
# --console keeps the kernel text drawn over the GUI (sweetfb_noconsole=1 is
# left out); without it the log is still in the ring at the end of userdata
# (tools/sweet-log-pull.sh from recovery).
# --gpu-fw DIR puts the Adreno firmware in DIR (a630_sqe.fw, and
# a610_zap.mdt / a610_zap.b02 if they are there) into the device tree, so
# patch 0145 finds it without reading a partition. Qualcomm's firmware is
# not in this repository: a630_sqe.fw is qcom/a630_sqe.fw of linux-firmware,
# the zap shader exists only on the phone.
# sweettouch_rx/ry: sapphire raw touch range for patch 0140 (X 0..2159,
# Y 0..2399, two fraction bits); kernels without 0140 ignore them.
# KERNEL is kernel.development.sweet from the xnu workflow artifact. The ADT
# takes /product from research/ipsw-x/d22ap.adt (extracted from the IPSW,
# not in git). The shim fits the RAM to the DDR bank ABL reports.
set -euo pipefail
cd "$(dirname "$0")/.."

rw="ufsrw=1 rootrw=1"
con="sweetfb_noconsole=1"
gpufw=()
while [[ ${1:-} == --* ]]; do
	case $1 in
	--readonly) rw="" ;;
	--console) con="" ;;
	--gpu-fw)
		shift
		[[ -f $1/a630_sqe.fw ]] || { echo "$1/a630_sqe.fw not found" >&2; exit 1; }
		gpufw=(--gpu-sqe "$1/a630_sqe.fw")
		[[ -f $1/a610_zap.mdt ]] && gpufw+=(--gpu-zap-mdt "$1/a610_zap.mdt")
		[[ -f $1/a610_zap.b02 ]] && gpufw+=(--gpu-zap-seg "$1/a610_zap.b02")
		;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
kernel=$(realpath "$1")
out=${2:-shim/out/sapphire-boot.img}
extra=${3:-}

[[ -f research/ipsw-x/d22ap.adt ]] || { echo "missing research/ipsw-x/d22ap.adt" >&2; exit 1; }
mkdir -p shim/out
python3 tools/mkadt.py shim/out/sapphire.adt --highmem ${gpufw[@]+"${gpufw[@]}"}
python3 tools/mkpayload.py "$kernel" shim/out/sapphire.adt shim/out/payload.bin \
	--cmdline "debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin $rw launchd_unsecure_cache=1 wdt=-1 ufslogsecs=1 $con sweetfb_hz=60 sweettouch_rx=8640 sweettouch_ry=9600 $extra"
shim/build.sh ../shim/out/payload.bin
[[ $out == shim/out/shim-boot.img ]] || cp shim/out/shim-boot.img "$out"
sha256sum "$out"
