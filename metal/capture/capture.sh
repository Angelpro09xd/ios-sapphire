#!/bin/sh
# Runs out/cap on turnip under drm-shim as sapphire's Adreno 610 and keeps the dump.
#   capture.sh NAME   -> out/NAME.rd (gzip) and out/NAME.txt (cffdump)
set -e
cd "$(dirname "$0")"
B=${MESA_BUILD:-/serverhive/angelpro09devv/mesa-build-vk}
NAME=${1:-draw}
TAG=sapphire_$$
FD_GPU_ID=610 FD_VA_START=${FD_VA_START:-0x30000000} FD_VA_SIZE=${FD_VA_SIZE:-0x08000000} \
TU_DEBUG=sysmem FD_RD_DUMP=enable,full FD_RD_DUMP_TESTNAME=$TAG \
LD_PRELOAD=$B/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so \
    ./out/cap $B/src/freedreno/vulkan/libvulkan_freedreno.so
mv /tmp/${TAG}_*.rd out/$NAME.rd   # turnip writes to /tmp, always
$B/src/freedreno/decode/cffdump --no-color --no-pager out/$NAME.rd > out/$NAME.txt 2>&1
echo "out/$NAME.rd, out/$NAME.txt"
