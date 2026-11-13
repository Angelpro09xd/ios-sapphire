# The Sapphire installer (developer notes)

End users only run `SapphireInstaller.exe` (see [docs/INSTALL.md](../docs/INSTALL.md)). This folder builds it and
everything it downloads. Everything builds on Linux (WSL on the maintainer's PC); users need nothing but Windows.

## How an install works
```
Windows app (C#)                         phone in OrangeFox (static aarch64 tools from the recovery ZIP)
----------------                         -----------------------------------------------------------
fetch release assets, adb/fastboot
adb push + `twrp install` the ZIP  --->  /tmp/sapphire: sapphire.sh, bin/, recipes/, screen/
sapphire.sh screen-on              --->  OrangeFox's UI stopped; sapphire-screen draws logo + status
sapphire.sh check / part / format  --->  GPT split (sapphire-part), fresh HFSX volume (mkfs.hfsplus)
IPSW images + overlay over         --->  sapphire-recv on 127.0.0.1:5550 (adb forward), CRC32-checked;
  adb forward tcp:5550                     the IPSW's images go to the volume's free tail
sapphire.sh rtc / build            --->  apfs-fuse mounts the images; the overlay is unpacked
                                          (sapphire-untar); recipes change Apple's files (plistop,
                                          heic2png); mkhfs writes the whole iOS root in one pass
kernel over TCP                    --->  padded to 128 MiB, os_version set, written to boot_<iOS slot>
fastboot set_active (optional)
```
Lines starting `STEP`, `PROGRESS done total`, `ERROR` are the protocol between the two
(`recovery/sapphire.sh`).

## Pieces
| Path | What |
|---|---|
| `windows/` | The installer (C#, .NET Framework 4.8, built with Windows' own `csc`, C# 5). `build.sh`. Dev flags: `--local DIR` (assets from a folder), `--shots DIR`, `--check-ipsw FILE` |
| `recovery/` | `sapphire.sh` (the phone side), `firstboot.sh` (iOS's first boot), `screen.c`, `loop.c`, `recv.c`, `untar.c`, `mkscreen.py`, `update-binary`, `build-zip.sh` → `sapphire-recovery.zip` |
| `mkhfs/` | Writes a complete HFSX volume from layered directories (base, grafts, unions, overlays, excludes) |
| `part/` | `sapphire-part`: the iosroot/userdata split in LUN 0's GPT |
| `plistop/` | Binary/XML plist editor for the recipes |
| `recipes/` | launchd jobs (`launchd/*.plist`), lists (120 Hz apps, removed extensions and jobs), `recipes.d/*.sh` |
| `overlay/` | `make-overlay.sh` (reads a reference phone's iOS root from Android), `scrub.py` (secrets, per-phone data, third-party packages out), `extras.sh` (Metal bundle, audio driver, Huella built from this repo) |
| `tools-arm64/` | Static aarch64 builds of apfs-fuse, mkfs.hfsplus (hfsprogs) and `heic/` (libheif/libde265) |

## Building a release
```sh
bash tools-arm64/build.sh && bash tools-arm64/heic/build.sh   # once
bash recovery/build-zip.sh                                     # sapphire-recovery.zip
bash overlay/make-overlay.sh                                   # phone in Android, rooted, with a reference iOS install
bash windows/build.sh                                          # SapphireInstaller.exe
bash ../tools/xnu-linux/kernel.sh OUT.img BASE.img             # kernel image (see tools/xnu-linux)
bash ../android/dualboot/build.sh                              # SapphireDualBoot.apk
```
Release assets: `SapphireInstaller.exe`, `sapphire-recovery.zip`, `sapphire-overlay.tar.gz`,
`sapphire-kernel.img`, `SapphireDualBoot.apk`. The installer takes the newest GitHub release (pre-releases
included) that has `sapphire-recovery.zip`.

## Testing without a phone
`recovery/sapphire.sh` honours `SAPPHIRE_DIR`, `SAPPHIRE_IOS` (an image file instead of the partition) and
`SAPPHIRE_PROP_*` (getprop stand-ins), so the whole format / recv / build path runs on a Linux PC with x86
builds of the tools, and the result can be mounted and checked with `fsck.hfsplus`.

## Rules
- Never redistribute Apple's files: everything from Apple comes from the user's IPSW, changed by recipes.
- Never write `persist`, the modem, RPMB or other LUNs; `persist` is only read (sensor calibration).
- No personal or per-phone data in the overlay (`overlay/scrub.py`).
