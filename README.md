# ios-sapphire

**Apple's iOS 16 running on a Redmi Note 13 4G.**

A port of [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (itself a port of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)) to the Xiaomi **Redmi Note 13 4G** (`sapphire` / `sapphiren`, Qualcomm **SM6225 "khaje"** / Snapdragon 685). Apple's open-source XNU kernel (xnu-8796) boots from RAM through a small boot shim, and on top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd and SpringBoard.

The Redmi Note 12 4G (topaz) and the Redmi Note 13 4G share the SoC, the memory carve-outs and most of the board, so this fork follows the topaz branch and adds what differs on sapphire (patches 0140–0146, the shim's DDR handling and the tools below).

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**. The kernel is started from RAM with `fastboot boot`; boot partitions do not need to be flashed.

## What works on sapphire

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi ABL (unlocked) → `fastboot boot` of the shim (boot image v4) → XNU on the four Gold cores. The shim fits the RAM to the DDR bank ABL reports |
| Storage | UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write. Shutdown from Settings unmounts it cleanly |
| GUI | Apple logo, lock screen, home screen, Settings, notification and control centers; rendered by the CPU (no GPU) |
| Touch | FocalTech FT3519T (chip id `54:52`) on QUP0 SE2, calibrated for sapphire by boot args (patch 0140) |
| USB | DWC3 in device mode as a CDC-ECM network card (patch 0142): `192.168.7.2` on the phone, live kernel log over UDP |
| Modem | Firmware read from the stock `modem_a`, authenticated by TrustZone, modem up with its QMI services (patch 0143) |
| Wi-Fi | WCN3988 firmware in mission mode and passive scan (2.4 and 5 GHz networks listed in the kernel log). No association yet |

## What does not work (yet)

- **Wi-Fi connection**: no association/WPA2, no data path, no network interface for iOS yet; the Wi-Fi switch in Settings does nothing.
- **No GPU in use yet**: everything is drawn by the CPU, so it lags; wallpapers and Metal effects stay black. Patch 0145 brings the Adreno 610 up, **it executes commands and it has drawn**: zap shader through TrustZone, SQE microcode, `CP_ME_INIT`, and a 256×256 solid fill with the 2D blitter that the CPU reads back with every pixel right (see "GPU bring-up"); nothing drives it from user space yet. The upstream project has the GPU drawing on topaz with patches it has not published.
- **SSH**: a Procursus rootless bootstrap with sshd can be installed (`tools/sapphire-ssh-install.sh`), but iOS does not resolve `root` for it yet ("Invalid user root").
- **No Bluetooth, cellular data, audio, camera, sensors, GPS, SEP**; no real-time clock (the date starts at 31 December).
- **Only 4 of 8 cores** (the Silver cores have no 16K translation granule).
- **Security is off**: permissive AMFI and sandbox, stub AppleKeyStore, no data protection.

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot sapphire-boot.img     (boot image v4, RAM only)
     -> shim on Silver CPU0: wakes Gold CPU 0x100, parks
        -> shim on Gold: GIC setup, DDR bank from ABL's DTB, payload behind _end
           -> XNU Mach-O + Apple Device Tree (ADT) + boot-args
              -> XNU -> UFS driver -> HFS+ root in userdata
                 -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

There is no serial console. The kernel keeps a persistent log ring at the end of `userdata` (read it from recovery with `tools/sweet-log-pull.sh`) and, with `sweetusb=2`, streams its console over USB. With `--console`, `tools/sapphire-boot.sh` also draws it over the GUI.

## How to do it yourself

You need a Redmi Note 13 4G (`sapphire`/`sapphiren`) with an **unlocked bootloader**, a recovery with a root adb shell (OrangeFox), a Linux machine (or WSL2) and a way to build the kernel on macOS. **Everything on `userdata` is lost.**

### 1. Build the kernel

XNU only builds with Apple's toolchain. Two ways:

- **Codemagic** (`codemagic.yaml`, free personal tier: 500 macOS minutes a month, a build takes a few minutes): add this repository as an application, start the `xnu` workflow on `sapphire`, download `xnu-kernel.zip`. It uses Xcode 16; patch 0141 makes the kernel build with it.
- **GitHub Actions** (`.github/workflows/xnu.yml`, macos-14 / Xcode 15):

  ```sh
  gh workflow run xnu.yml -R YOU/ios-sapphire --ref sapphire
  gh run download RUN_ID -R YOU/ios-sapphire -n xnu-kernel -D xnu/out
  ```

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple and extract the D22AP Erase identity's OS, Cryptex1 SystemOS and AppOS disk images (with `apfs-fuse`/`ipsw`). Then, on Linux or WSL2 as root (WSL2 needs an `hfsplus.ko` built for its kernel):

```sh
sudo tools/sapphire-root.sh OS.dmg SystemOS.dmg AppOS.dmg OUTDIR [hfsplus.ko]
# -> OUTDIR/ios16-root.img (16 GB) and ios16-root.simg
```

The device tree builder copies `/product` from the real iPhone X device tree: take `Firmware/all_flash/DeviceTree.d22ap.im4p` from the IPSW, decompress it (`ipsw img4 im4p extract`) and save it as `research/ipsw-x/d22ap.adt`.

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and AOSP `mkbootimg.py` (`MKBOOTIMG=...`).

```sh
tools/sapphire-boot.sh xnu/out/kernel.development.sweet sapphire-boot.img            # GUI
tools/sapphire-boot.sh xnu/out/kernel.development.sweet sapphire-usb.img "sweetusb=2" # + USB network
tools/sapphire-boot.sh xnu/out/kernel.development.sweet sapphire-wifi.img "sweetusb=2 modem=1 wifi=1"
```

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on `userdata`, writable (`--readonly` drops the last two) |
| `cpus=4` | The four Gold cores |
| `sweetfb_noconsole=1` | No kernel text over the GUI (default; `--console` keeps it) |
| `sweetfb_hz=60` | Panel refresh rate of the CPU frame copy |
| `sweettouch_rx=8640 sweettouch_ry=9600` | sapphire's raw touch range (X 0..2159, Y 0..2399, 2 fraction bits) |
| `sweetusb=1` / `sweetusb=2` | USB: report the controller state / start the CDC-ECM device |
| `modem=1 wifi=1` | Boot the modem from `modem_a` and bring up the WLAN firmware (read-only on the phone's partitions) |
| `gpu=1` / `2` / `3` | Adreno 610: log the clock tree / also power it up and read it / also run a command on it. `gpu_delay`, `gpu_slow`, `gpu_div` and `gpu_smmu_early` go with them (see "GPU bring-up") |
| `launchd_unsecure_cache=1`, `wdt=-1 wdt_test=no_user_checkin` | launchd without AMFI; no hardware watchdog |

### 4. Put the root on the phone and boot

`fastboot flash userdata` of a sparse image wrote the upper part of `userdata` incorrectly on sapphire's ABL, so copy the raw image from recovery instead (root shell, `userdata` = `/dev/block/by-name/userdata`):

```sh
adb shell 'dd of=/dev/block/by-name/userdata bs=1M' < ios16-root.img    # Linux adb; verify with md5sum
adb reboot bootloader
fastboot boot sapphire-boot.img
```

Use the Linux `adb` for this: piping binary data into Windows' `adb.exe shell` corrupts it. From Windows, `adb push` the image in pieces and `dd` them on the phone. Check the result by comparing md5 sums of 1 MB blocks on both sides.

Always use `fastboot boot`, not `fastboot flash boot`: on sapphire the recovery runs with the kernel in `boot`, so an iOS image in `boot` takes the recovery away (see `RECOVERY.md`). The first boot spends a while on the Apple logo; the lock screen follows after a few minutes. To leave iOS cleanly use Settings → General → Shut Down (the HFS+ journal stays clean), then Power + Volume down.

### USB network and live log

With `sweetusb=2` the phone appears as "Sweet iOS (USB Ethernet)". Windows has no CDC-ECM driver; pass it to WSL2 with [usbipd-win](https://github.com/dorssel/usbipd-win) (`usbipd bind --busid X` once, `usbipd attach --wsl --busid X` per boot), then in Linux:

```sh
sudo ip addr add 192.168.7.1/24 dev enx02005e77ee01 && sudo ip link set enx02005e77ee01 up
ping 192.168.7.2
nc -ukl 5140          # live kernel console
```

### Changing files on the phone without a full copy

`tools/hfs-push-ranges.py` and `tools/sapphire-ssh-install.sh` edit the root without rewriting 16 GB: copy the phone's HFS+ metadata ranges (volume headers, journal header, catalog, extents, attributes, bitmap) into the image on the PC, change files there (new files only, replace through new file + rename), and write back only the newly allocated blocks plus the metadata, with `adb push` + `dd` on the phone. The journal must be clean first (shut down from Settings). `tools/bplist-same-size.py` drops launchd jobs from `launchd.plist` in place.

## sapphire patches and tools

| # / file | What |
| --- | --- |
| 0140 | Focaltech raw format and range from boot args, first raw contacts logged, out-of-range positions clamped instead of dropped |
| 0141 | `IORPC.kernelContent` backport so the kernel builds with Xcode 16's `iig` |
| 0142 | sweet's DWC3/CDC-ECM USB device on the SM6225 addresses; opt-in with `sweetusb` |
| 0143 | sapphire's modem_a tables (modem image for PAS, all files for the tftp server and WLAN board data), picked by the `modem.mdt` CRC so topaz keeps working |
| 0144 | Join the network named by boot args `wifi_ssid=` / `wifi_pass=` through upstream's WLAN station (0129/0130), for testing |
| 0145 | Adreno 610 bring-up staged by boot-arg `gpu=N`: clock tree, power-up, zap shader through TrustZone, identity SMMU bank, SQE microcode and a command the GPU executes (powered and answering on the phone; the command stages are blocked by the panic below) |
| 0146 | Black box: a kernel panic writes the console ring (with the panic text) to the end of `userdata` before the panic path spins, since this board has no iBoot panic region. `tools/sweet-log.py` reports it as a PANIC flush |
| `shim/` | DDR bank end from ABL's `/memory` node; RAM size clamped to it |
| `tools/mkadt.py` | sapphire identity in the ADT |
| `tools/sapphire-boot.sh`, `sapphire-root.sh` | Boot image and root image for sapphire |
| `tools/fwtable-modem.py` | Generates the modem_a tables from a dump of the partition |
| `tools/fwtable-file.py` | Where a file lies inside a partition, for a driver that reads it directly (the GPU firmware table) |
| `tools/sapphire-touch-kpatch.sh`, `kpatch.py` | Binary fix of the touch scaling in a built kernel (used before a build was possible) |

## GPU bring-up (patch 0145, in progress)

The Adreno 610 v2 in this phone (`chip id 0x06010001`, registers at `0x5900000`) has no GMU, so
the host powers it up itself, as Linux does through its "GMU wrapper". Patch 0145 follows what
[topaz-woa](https://github.com/MakrSas/topaz-woa) `docs/P8_gpu.md` established on the same SoC
under Windows, on this kernel and with the pieces `TopazModem` already brings: TrustZone calls,
an identity SMMU bank and RAM below 4 GB that the kernel does not manage.

Reading a GPU register while its power domain or its clocks are off hangs the bus, so the work is
staged by `gpu=N` and each stage is meant to be read in the kernel log before going further:

| Stage | What it does | What to look for |
| --- | --- | --- |
| `gpu=1` | Maps GCC and the GPU CC and logs the clock tree. Never touches `0x5900000` | the bootloader leaves everything collapsed and off |
| `gpu=2` | Powers up (GPLL0 vote, CX GDSC, cxo/ahb, GMU RCG, SMMU vote, GX GDSC, core clock at 300 MHz), then reads the GPU | `RBBM_STATUS`, and `CP_ALWAYS_ON_COUNTER` growing by ~192000 in 10 ms (19.2 MHz) |
| `gpu=3` | Zap shader through TrustZone (PAS 13, region `0x55A15000` from the device tree), identity SMMU bank for the GPU's stream (SID 0 mask 1, as `gfx3d_user` in the device tree), SQE microcode, 32 KB ring, `CP_ME_INIT`, `CP_SET_SECURE_MODE 0`, `CP_MEM_WRITE` | `*** the GPU executes commands ***`, with `c0ffee00` read back from RAM |
| `gpu=4` | The same without the zap shader: leaves secure mode by writing `RBBM_SECVID_TRUST_CNTL`, as Linux does for a device that has no zap firmware. A first test with only the open microcode, and a gamble Linux warns about: if the bootloader left the GPU secure, that write is refused | the same, or a permissions violation |

Stage 2 works: the GPU is powered and its always-on counter runs. What is not solved is that
most boots which get that far **kill the phone about a second later**, with every clock reading
what it should and nothing else in the log. Four more boot args exist for that, and the three
after `gpu_delay` default to the behaviour that has already run, so a boot changes one thing:

| Arg | What it does |
| --- | --- |
| `gpu_delay=N` | wait N seconds before starting, so the bring-up log goes out over the live USB console instead of only into the ring at the end of `userdata` |
| `gpu_slow=N` | wait N ms after every step (1200 works), so the once-a-second log writer puts each step on disk before the next one runs |
| `gpu_div=N` | run the core at GPLL0/N: 2 = 300 MHz (the default), 8 = 75 MHz |
| `gpu_smmu_early=1` | program the identity SMMU bank as soon as its clock is voted, before the GX domain, so the GPU never sees the bus without a context bank |
| `gpu_stop=N` | end stage 3 early: 1 after the zap shader, 2 once the SQE runs, 3 after `CP_ME_INIT`, 4 after `CP_SET_SECURE_MODE` |
| `gpu_keep=1` | leave the GPU running at the end; by default it is parked (GBIF bus drained, GX and CX collapsed) |
| `gpu_draw=0` | skip the first draw (a 256×256 solid fill with the 2D blitter, read back by the CPU) that stage 3 now does after its test |
| `gpu_irq=1` | leave the GPU's interrupt (SPI 177, INTID 209) alone. By default it is masked in the GIC distributor before anything is powered, and at the GPU once its registers answer: it is level triggered and nothing here handles it, which is what the deaths above look like |

Both `gpu=3` and `gpu=4` need the microcode `a630_sqe.fw`, and `gpu=3` also needs this phone's
signed `a610_zap` shader. Both come off the phone's own `/vendor/firmware`, which is an EROFS
logical partition in `super`, in slot `_b` on this device (`HANDOFF.md` has the three commands);
the microcode is also in [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) as
`qcom/a630_sqe.fw`, in a different build (34188 bytes against the vendor's 32504). Neither is in
this repository, and neither should be put in it. The simplest way to give them to the driver is
to put them in the boot image's device tree:

```sh
tools/sapphire-boot.sh --gpu-fw ~/gpu-fw xnu/out/kernel.development.sweet sapphire-gpu.img "gpu=4"
```

where `~/gpu-fw` holds `a630_sqe.fw` (and `a610_zap.mdt` / `a610_zap.b02` for `gpu=3`). Otherwise
the driver reads them from a stock partition: fill `gpu_fw[]` in `SapphireGpu.cpp` with
`tools/fwtable-file.py`, pointing it at the files in a mounted copy of that partition.

After that come the interrupt, a real draw from a freedreno command stream, and a user space
driver (Mesa freedreno) behind CoreAnimation.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), boot image v4, Gold hand-off, GIC setup, payload loader |
| `xnu/patches/` | XNU patch series, applied in order: sweet's 0001–0079, topaz's 0080–0139, sapphire's 0140+ |
| `codemagic.yaml`, `.github/workflows/xnu.yml` | Kernel builds |
| `tools/` | ADT, payload, root image, boot image, log, modem and HFS+ tools |
| `NOTES.md` | Engineering log (sweet, topaz, sapphire) |
| `RECOVERY.md` | Getting back to recovery / Android on sapphire |

Not in git: partition dumps, live device trees, Apple firmware and root images, boot logs, build trees.

## Key hardware facts (sapphire)

- SoC SM6225 "khaje": 4× Kryo 260 Silver (A53 class, MPIDR `0x0`–`0x3`) + 4× Kryo 260 Gold (A73 class, `0x100`–`0x103`). Only Gold has the 16K granule.
- DDR banks of the tested phone (from ABL's DTB): `0x40000000`+`0x3cc00000`, `0x80000000`+1 GiB, `0xc0000000`+4 GiB. XNU gets 2 GiB at `0x1_40000000`.
- GICv3 at `0xf200000` (redistributors `0xf300000`), UFS at `0x4804000`, splash framebuffer at `0x5c000000` (1080×2400, DSI video mode with DSC).
- Touch: FocalTech FT3519T at `0x38` on QUP0 SE2 (IRQ GPIO80, reset GPIO86), raw range 2160×2400.
- USB: DWC3 at `0x4e00000` (QSCRATCH `+0xf8800`), Synopsys femto HS PHY at `0x1613000`, USB30_PRIM GDSC at GCC `0x1400000` + `0x1a004`.
- Modem carve-out `0x4ab00000`+`0x6900000`, WLAN MSA `0x51900000`+`0x100000`, SMEM `0x46000000`; WCN3988 (`chip_id 150`) behind the modem.

## Credits

Based on [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) and [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the hard part: XNU on a Qualcomm phone with the iPhone X userland, the topaz port of the same SoC, its modem and Wi-Fi work. Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), [Procursus](https://github.com/ProcursusTeam/Procursus), Qualcomm/Xiaomi's published Linux kernel sources, and the hardware work in [MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa). sapphire port by [Angelpro09xd](https://github.com/Angelpro09xd). iOS itself is Apple's and is not distributed here.
