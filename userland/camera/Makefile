# Sapphire camera pipeline.
#   make            host build (WSL/Linux) into build/host/
#   make ios        arm64 iOS build (clang 18 + iPhoneOS SDK, ldid-signed) into build/ios/
#   make test       quick round-trip tests on the host (tests/run.sh)
SDK   ?= $(HOME)/sdks/theos-sdks/iPhoneOS16.5.sdk
LDID  ?= $(HOME)/tools-dl/ldid/ldid
CLANG ?= /usr/lib/llvm-18/bin/clang

CFLAGS_COMMON = -O3 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -fno-math-errno
HOST_CC     ?= $(CLANG)
HOST_CFLAGS ?= $(CFLAGS_COMMON) -march=native
IOS_CC       = $(CLANG) -target arm64-apple-ios15.0 -isysroot $(SDK) -fuse-ld=lld
IOS_CFLAGS   = $(CFLAGS_COMMON) -mcpu=cortex-a73

CORE = util.c stage.c rawio.c remosaic.c demosaic.c color.c out.c jpeg.c
HDRS = common.h stage.h rawio.h remosaic.h color.h out.h imgread.h

all: build/host/rawtool build/host/rawsynth

build/host/rawtool: $(CORE) rawtool.c $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(CORE) rawtool.c -lm -lpthread

build/host/rawsynth: util.c rawio.c stage.c imgread.c rawsynth.c $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ util.c rawio.c stage.c imgread.c rawsynth.c -lm -lpthread

ios: build/ios/rawtool build/ios/rawsynth

build/ios/rawtool: $(CORE) rawtool.c $(HDRS)
	@mkdir -p $(@D)
	$(IOS_CC) $(IOS_CFLAGS) -o $@ $(CORE) rawtool.c
	$(LDID) -S $@

build/ios/rawsynth: util.c rawio.c stage.c imgread.c rawsynth.c $(HDRS)
	@mkdir -p $(@D)
	$(IOS_CC) $(IOS_CFLAGS) -o $@ util.c rawio.c stage.c imgread.c rawsynth.c
	$(LDID) -S $@

test: all
	sh tests/run.sh

clean:
	rm -rf build

.PHONY: all ios test clean
