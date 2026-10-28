#!/bin/bash
# xfire.sh: libfirehose_kernel.a (libdispatch's xcodeconfig/libfirehose_kernel.xcconfig) into the fakeroot,
# with its private header os/firehose_buffer_private.h unifdef'd as Xcode's copy-headers phase does
set -e
L=~/xnu-local
export PATH=~/xnu-tools/bin:$PATH
D=$L/libdispatch
K=$L/dst/System/Library/Frameworks/Kernel.framework/Versions/A
F=$L/fakeroot
mkdir -p $F/usr/local/lib/kernel $F/usr/local/include/kernel/os $L/build/firehose
# headers the kernel side of firehose needs from libplatform (os/base_private.h, os/lock_private.h...)
mkdir -p $L/build/firehose/inc
cp -r $L/libplatform/private/os $L/build/firehose/inc/ 2>/dev/null || true
cp "$(xcrun --show-sdk-path)/usr/include/TargetConditionals.h" $L/build/firehose/inc/
unifdef -DKERNEL=1 -DOS_FIREHOSE_SPI=1 -DOS_VOUCHER_ACTIVITY_SPI_TYPES=1 -UOS_VOUCHER_ACTIVITY_SPI \
	$D/os/firehose_buffer_private.h > $F/usr/local/include/kernel/os/firehose_buffer_private.h || true
xclang -arch arm64 -mkernel -nostdinc -Wno-packed -mcpu=cortex-a53 -O2 -std=gnu11 \
	-DKERNEL=1 -DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 -DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 \
	-I$D -I$K/PrivateHeaders -I$K/Headers -I$L/dst/usr/local/include/os -I$L/dst/usr/local/include/firehose \
	-I$L/dst/usr/local/include -I$L/build/firehose/inc -I$L/xnu/EXTERNAL_HEADERS -I$F/usr/include \
	-c $D/src/firehose/firehose_buffer.c -o $L/build/firehose/firehose_buffer.o
rm -f $F/usr/local/lib/kernel/libfirehose_kernel.a
libtool -static -o $F/usr/local/lib/kernel/libfirehose_kernel.a $L/build/firehose/firehose_buffer.o
nm $F/usr/local/lib/kernel/libfirehose_kernel.a | grep ' T ' | head
