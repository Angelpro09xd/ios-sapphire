#!/bin/bash
# xmake.sh TARGET [extra make args]: xnu's make on Linux with the tools in ~/xnu-tools/bin, for the phone's
# kernel (DEVELOPMENT ARM64 SWEET, 4 KiB pages). TARGET installhdrs or install. Log in ~/xnu-local/TARGET.log; XNU_JOBS (12: WSL fell over with more).
L=~/xnu-local
T=$1; shift
export PATH=~/xnu-tools/bin:$PATH
export TOPAZ_4K=1
FAKEROOT=$L/fakeroot
mkdir -p $FAKEROOT/usr/local/libexec
cp -n $L/AvailabilityVersions/availability.pl $FAKEROOT/usr/local/libexec/ 2>/dev/null; chmod +x $FAKEROOT/usr/local/libexec/availability.pl
case $T in
installhdrs) OBJ=$L/build/hdrs.obj SYM=$L/build/hdrs.sym CFG='ARCH_CONFIGS=ARM64' ;;
*) OBJ=$L/build/xnu.obj SYM=$L/build/xnu.sym CFG='TARGET_CONFIGS=DEVELOPMENT ARM64 SWEET' ;;
esac
cd $L/xnu
make $T -j"${XNU_JOBS:-12}" "$CFG" SDKROOT=macosx HOST_SDKROOT=macosx XCRUN=$HOME/xnu-tools/bin/xcrun \
	HOST_CC=clang-18 HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 \
	HOST_CODESIGN=/bin/true HOST_CODESIGN_ALLOCATE=$HOME/xnu-tools/bin/codesign_allocate \
	MIG=$HOME/xnu-tools/bin/mig MIGCOM=$HOME/xnu-tools/bin/migcom MIGCC=$HOME/xnu-tools/bin/xclang \
	IIG=$HOME/xnu-tools/bin/iig PLUTIL=$HOME/xnu-tools/bin/plutil \
	LD_KERNEL_LIBS=$FAKEROOT/usr/local/lib/kernel/libcc_kext.a \
	CTFCONVERT= CTFMERGE= CTFINSERT= CTFDUMP= DO_CTFMERGE=0 \
	LOGCOLORS=n BUILD_WERROR=0 BUILD_LTO=0 VERBOSE=YES \
	SRCROOT=$L/xnu OBJROOT=$OBJ SYMROOT=$SYM DSTROOT=$L/dst FAKEROOT_DIR=$FAKEROOT \
	RC_DARWIN_KERNEL_VERSION=22.6.0 MEMORY_SIZE=8589934592 SYSCTL_HW_PHYSICALCPU=8 SYSCTL_HW_LOGICALCPU=8 \
	KERNEL_BUILDS_IN_PARALLEL=1 "$@" > $L/$T.log 2>&1
r=$?
echo "make $T exit $r"
grep -n -m 30 -e 'error:' -e '\*\*\*' -e 'Error [0-9]' $L/$T.log | cut -c1-400
exit $r
