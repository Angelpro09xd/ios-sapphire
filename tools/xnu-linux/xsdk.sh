#!/bin/bash
# xsdk.sh: ~/xnu-tools/MacOSX.sdk = our iPhoneOS 16.5 SDK as symlinks, with a macOS TargetConditionals.h (the iOS one
# hard-codes TARGET_OS_IOS=1; clang 18 predefines the right TARGET_OS_* for an apple-macos target, so the header only
# fills in what it does not)
set -e
T=~/xnu-tools
S=$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk
rm -rf $T/MacOSX.sdk
cp -as "$S" $T/MacOSX.sdk
rm $T/MacOSX.sdk/usr/include/TargetConditionals.h
cat > $T/MacOSX.sdk/usr/include/TargetConditionals.h <<'EOF'
/* macOS TargetConditionals.h for building xnu with LLVM clang on Linux (arm64-apple-macos target) */
#ifndef __TARGETCONDITIONALS__
#define __TARGETCONDITIONALS__
#ifndef TARGET_OS_MAC
#define TARGET_OS_MAC 1
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 1
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#ifndef TARGET_OS_IOS
#define TARGET_OS_IOS 0
#endif
#ifndef TARGET_OS_WATCH
#define TARGET_OS_WATCH 0
#endif
#ifndef TARGET_OS_TV
#define TARGET_OS_TV 0
#endif
#ifndef TARGET_OS_MACCATALYST
#define TARGET_OS_MACCATALYST 0
#endif
#ifndef TARGET_OS_UIKITFORMAC
#define TARGET_OS_UIKITFORMAC 0
#endif
#ifndef TARGET_OS_SIMULATOR
#define TARGET_OS_SIMULATOR 0
#endif
#ifndef TARGET_OS_EMBEDDED
#define TARGET_OS_EMBEDDED 0
#endif
#ifndef TARGET_OS_RTKIT
#define TARGET_OS_RTKIT 0
#endif
#ifndef TARGET_OS_DRIVERKIT
#define TARGET_OS_DRIVERKIT 0
#endif
#ifndef TARGET_OS_BRIDGE
#define TARGET_OS_BRIDGE 0
#endif
#ifndef TARGET_OS_XR
#define TARGET_OS_XR 0
#endif
#ifndef TARGET_OS_VISION
#define TARGET_OS_VISION 0
#endif
#define TARGET_OS_NANO TARGET_OS_WATCH
#define TARGET_OS_WIN32 0
#define TARGET_OS_WINDOWS 0
#define TARGET_OS_UNIX 0
#define TARGET_OS_LINUX 0
#define TARGET_OS_EMBEDDED_OTHER 0
#define TARGET_IPHONE_SIMULATOR TARGET_OS_SIMULATOR
#define TARGET_OS_NANO TARGET_OS_WATCH
#define TARGET_CPU_PPC 0
#define TARGET_CPU_PPC64 0
#define TARGET_CPU_68K 0
#define TARGET_CPU_X86 0
#define TARGET_CPU_X86_64 0
#define TARGET_CPU_ARM 0
#define TARGET_CPU_ARM64 1
#define TARGET_CPU_MIPS 0
#define TARGET_CPU_SPARC 0
#define TARGET_CPU_ALPHA 0
#define TARGET_RT_MAC_CFM 0
#define TARGET_RT_MAC_MACHO 1
#define TARGET_RT_LITTLE_ENDIAN 1
#define TARGET_RT_BIG_ENDIAN 0
#define TARGET_RT_64_BIT 1
#endif /* __TARGETCONDITIONALS__ */
EOF
echo '#include <TargetConditionals.h>
TARGET_OS_IOS TARGET_OS_OSX TARGET_CPU_ARM64' | $T/bin/xclang -arch arm64 -isysroot $($T/bin/xcrun --show-sdk-path) -E - 2>&1 | tail -1
