# ios-sapphire

**Apple's iOS 16 running on a Redmi Note 13 4G.**

A port of [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (itself a port of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)) to the Xiaomi **Redmi Note 13 4G** (`sapphire` / `sapphiren`, Qualcomm **SM6225 "khaje"** / Snapdragon 685). Apple's open-source XNU kernel (xnu-8796) boots from RAM through a small boot shim, and on top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd and SpringBoard.

The Redmi Note 12 4G (topaz) and the Redmi Note 13 4G share the SoC, the memory carve-outs and most of the board, so this fork follows the topaz branch and adds what differs on sapphire (patches 0127–0130, the shim's DDR handling and the tools below).

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**. The kernel is started from RAM with `fastboot boot`; boot partitions do not need to be flashed.

## What works on sapphire

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi ABL (unlocked) → `fastboot boot` of the shim (boot image v4) → XNU on the four Gold cores. The shim fits the RAM to the DDR bank ABL reports |
| Storage | UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write. Shutdown from Settings unmounts it cleanly |
| GUI | Apple logo, lock screen, home screen, Settings, notification and control centers; rendered by the CPU (no GPU) |
| Touch | FocalTech FT3519T (chip id `54:52`) on QUP0 SE2, calibrated for sapphire by boot args (patch 0127) |
| USB | DWC3 in device mode as a CDC-ECM network card (patch 0129): `192.168.7.2` on the phone, live kernel log over UDP |
| Modem | Firmware read from the stock `modem_a`, authenticated by TrustZone, modem up with its QMI services (patch 0130) |
| Wi-Fi | WCN3988 firmware in mission mode and passive scan (2.4 and 5 GHz networks listed in the kernel log). No association yet |

## What does not work (yet)

- **Wi-Fi connection**: no association/WPA2, no data path, no network interface for iOS yet; the Wi-Fi switch in Settings does nothing.
- **No GPU**: everything is drawn by the CPU, so it lags; wallpapers and Metal effects stay black.
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

- **Codemagic** (`codemagic.yaml`, free personal tier: 500 macOS minutes a month, a build takes a few minutes): add this repository as an application, start the `xnu` workflow on `sapphire`, download `xnu-kernel.zip`. It uses Xcode 16; patch 0128 makes the kernel build with it.
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
| 0127 | Focaltech raw format and range from boot args, first raw contacts logged, out-of-range positions clamped instead of dropped |
| 0128 | `IORPC.kernelContent` backport so the kernel builds with Xcode 16's `iig` |
| 0129 | sweet's DWC3/CDC-ECM USB device on the SM6225 addresses; opt-in with `sweetusb` |
| 0130 | sapphire's modem_a tables (modem image for PAS, all files for the tftp server and WLAN board data), picked by the `modem.mdt` CRC so topaz keeps working |
| `shim/` | DDR bank end from ABL's `/memory` node; RAM size clamped to it |
| `tools/mkadt.py` | sapphire identity in the ADT |
| `tools/sapphire-boot.sh`, `sapphire-root.sh` | Boot image and root image for sapphire |
| `tools/fwtable-modem.py` | Generates the modem_a tables from a dump of the partition |
| `tools/sapphire-touch-kpatch.sh`, `kpatch.py` | Binary fix of the touch scaling in a built kernel (used before a build was possible) |

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), boot image v4, Gold hand-off, GIC setup, payload loader |
| `xnu/patches/` | XNU patch series, applied in order: sweet's 0001–0079, topaz's 0080–0126, sapphire's 0127+ |
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
