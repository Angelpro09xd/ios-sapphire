# Building the sapphire kernel on Linux

The xnu kernel of the port (DEVELOPMENT ARM64 SWEET, 4 KiB pages, `xnu/patches`) built on Linux (WSL
Ubuntu 24.04 here) without macOS, Xcode or CI: ~8 minutes with `-j12` once the tools exist. It follows
PureDarwin's Linux xnu build (cctools ld64, mig, iig-lite, SETUP tools) with an LLVM clang 18.1.8 patched
for the builtins Apple's clang has and LLVM does not.

Downloads: the LLVM 18.1.8 llvm/clang/cmake/third-party sources (~70 MB), PureDarwin and iig-tools
(shallow), darwin-xnu-build at the CI's commit, and the macOS 13.5 tags of AvailabilityVersions, libdispatch
and libplatform. No macOS SDK: our iPhoneOS 16.5 SDK (`~/sdks/theos-sdks/iPhoneOS16.5.sdk`) stands in for it.

Layout: tools in `~/xnu-tools` (`bin/`, `llvm-src`, `llvm-build`, `MacOSX.sdk`), sources in `~/tools-dl/pd`,
the xnu tree and its build in `~/xnu-local`, xnu's pristine source (tag xnu-8796.141.3) in `~/xnu-src`.

## Once

    sudo bash aptdeps.sh             # cmake ninja bison flex unifdef tcsh ... (clang-18 and lld-18 from Ubuntu)
    bash mktools.sh                  # PureDarwin + iig-tools, bin/ wrappers into ~/xnu-tools/bin
    bash mkld64.sh && bash mkcct.sh  # ld64, strip, libtool, lipo, nm, nmedit, codesign_allocate, ctf_insert
    bash mkmig.sh                    # mig, migcom
    bash mkiig.sh                    # iig-lite (with iigfix.py)
    bash llvmget.sh && bash mkclang.sh   # clang with the xnu builtins (~1 h the first time)
    bash xsdk.sh                     # ~/xnu-tools/MacOSX.sdk
    bash xavail.sh && bash xdisp.sh  # AvailabilityVersions (availability.pl), libdispatch, libplatform

## Each kernel

    bash kernel.sh OUT.img [BASE.img]

prepares the tree exactly as CI does (`xall.sh`: `xprep.sh` applies darwin-xnu-build's and our patches with
`git apply --check`, `xsetup.sh`, headers, libfirehose_kernel, libcc_kext), runs `make install`
(`xmake.sh`, log in `~/xnu-local/install.log`) and repacks the kernel into a copy of BASE.img
(`tools/sapphire-repack.py`: same shim, DTB, ADT and boot-args). Only for `fastboot boot`.

## What differs from Apple's toolchain

- **clang** (`clang-xnu-builtins.py`): `__builtin_xnu_type_signature`, `__builtin_xnu_type_summary`,
  `__builtin_xnu_types_compatible` and `__attribute__((xnu_usage_semantics))` (kalloc_type, the vm_pageout.c
  static_assert); void* dereference in unevaluated C++; TreeTransform rebuilds array type traits on
  instantiation (else infinite recursion in IOUserServer.cpp). `xclang` adds `-D__apple_build_version__`
  (the arm64 SHA asm syntax) and `-mllvm -disable-atexit-based-global-dtor-lowering` (no `__cxa_atexit`).
- **libcc_kext.a** (`cc_kext.c`): stubs for the two `__llvm_profile_*` calls of OSKext.cpp, passed through
  `LD_KERNEL_LIBS` (the clang driver drops `-lcc_kext`).
- **MacOSX.sdk**: the iOS SDK's headers with a macOS TargetConditionals.h (the iOS one turns on
  CONFIG_PROTECT in HFS).
- **No CTF** (no ctfconvert/ctfmerge): the kernel has no `__CTF` section; nothing on the phone reads it.
- **iig-lite**, not Apple's iig: DriverKit glue differs (no OSAction_IOUserClient_KernelCompletion,
  `Create` vs `Create_Call`); `make install` then fails at the exports check after the kernel is linked.
  The kernel in `~/xnu-local/build/xnu.obj/DEVELOPMENT_ARM64_SWEET` is complete; only dexts (unused) would
  notice. `kernel.sh` takes it from there.

Kernel 0241 built this way (2026-10-28) boots the phone to SpringBoard with CommCenter and wifid up; next to
the CI's 0240 its segments match and it has 24449 global symbols vs 24471 (the DriverKit ones).
