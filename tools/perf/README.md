# Measurement tools for the phone

Small userland tools used to measure the kernel patches on the phone. Build them on a Mac with clang and an
iPhoneOS SDK, and sign them ad hoc:

```sh
SDK=/path/to/iPhoneOS16.5.sdk
clang -target arm64-apple-ios16.0 -isysroot $SDK -O2 -o thrtop thrtop.c && codesign -f -s - thrtop
clang -target arm64-apple-ios16.0 -isysroot $SDK -O2 -o gxfreq gxfreq.c -framework IOKit -framework CoreFoundation \
    && codesign -f -s - gxfreq
clang -target arm64-apple-ios16.0 -isysroot $SDK -O2 -o fbshot fbshot.c -framework IOSurface -framework CoreFoundation \
    && codesign -f -s - fbshot
```

| Tool | What it measures |
|---|---|
| `gxfreq [MS] [ROUNDS]` | The GPU core clock as the GPU counts it: CP and RBBM always-count cycles against the 19.2 MHz always-on counter, sampled by the CP with `CP_REG_TO_MEM` (Linux `msm_gpu.c` `retire_submit`). Needs `gpu=3 gpu_mmu=1 gpu_service=1` |
| `thrtop PID SECONDS` | CPU time (user and system) per thread of a process over the interval |
| `psamp PID SECONDS THREADNAME OUT [PERIOD_US]` | Stack samples (pc and frame-pointer backtrace) of one thread every 0.5 ms by default, plus the process's image list, for an offline profile |
| `fbshot PREFIX [W H]` | Writes every IOSurface of W x H (default 750 x 1624, the screen's swap chain) to `PREFIX-N.raw` |
| `raw2png.py RAW PNG W H` | On the Mac: turns an `fbshot` dump into a PNG |

With the panel asleep nothing is drawn and `fbshot` returns the last frame: wake the panel first.
