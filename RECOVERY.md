# topaz: back to stock / recovery runbook

Redmi Note 12 4G (`topaz`, ROM codename `tapas`), SM6225. Everything below runs on **s8build**
with the phone in **fastboot** (Power + Volume down). Bootloader is unlocked; there is no EDL.

Last verified working: 2026-10-19 (this exact sequence brought a phone that booted nothing back
to HyperOS).

## Files (on s8build)

| What | Path |
| --- | --- |
| Stock fastboot ROM HyperOS 2.0.208.0 Global (MD5 `322325af295dd7cc47e733dafbc63b9c`) | `/mnt/mui/ios-topaz/rom/tapas_global_images_OS2.0.208.0.VMTMIXM_20260720.0000.00_15.0_global_322325af29.tgz` |
| LUN0 GPT (userdata + **esp + win**) rebuilt from the 2026-10-01 backup | `/mnt/mui/ios-topaz/gpt_both0_20261001.bin` |
| LUN4 GPT (boot/vendor_boot/dtbo/... A/B), slot a active | `~/work/topaz/gpt_both4_20260930.bin` |
| Raw GPT heads of all LUNs: 2026-10-01 / 2026-10-19 | `~/work/topaz/gpt_backup_20261001/`, `/mnt/mui/winbackup/gpt_sd?.head.bin` |
| TWRP (tapas), goes to `recovery_a` | `~/twrp.img` |
| Mu-Silicium UEFI for Windows (`boot_b`) | `~/work/win/uefi/Mu-topaz-v10-RELEASE.img` (later test builds: `/mnt/mui/wpoa/`) |
| Windows backup (esp + win, zstd) | `/mnt/mui/winbackup/esp.img.zst`, `win.img.zst` (+ `.sha256` of the raw images) |
| Slot backups 2026-09-30 / 2026-10-01 | `~/work/topaz/backup/*.img.gz`, `~/work/topaz/backup_20261001/`, `~/work/topaz/stock/` |
| GPT tools | `/mnt/mui/ios-topaz/gptdump.py` (print slot GUIDs/attrs), `/mnt/mui/ios-topaz/mkgptboth.py` (build gpt_bothN.bin from head/tail dumps with CRC checks) |

## Current layout (2026-10-19)

- Slot **b** active: `boot_b` = iOS shim (`/mnt/mui/ios-topaz/shim-boot-0088.img`), boots iOS on power-on.
- `userdata` = iOS HFS+ root (`/mnt/mui/ios-topaz/ios16-root.simg`).
- Slot a = stock HyperOS 2.0.208 (needs userdata wiped to boot), `recovery_a` = TWRP.
- Windows data intact in `esp`/`win`; Mu is not installed (step 6 to boot Windows).
- Update iOS: `fastboot flash boot_b <new shim-boot.img>` (or test first with `fastboot boot`).

## 0. Diagnose first

```sh
fastboot getvar all 2>&1 | grep -E "current-slot|slot-|anti|unlocked"
for p in userdata super esp win persist; do fastboot getvar partition-size:$p; done
fastboot continue        # prints the real reason if the boot fails
```

- `partition-size:userdata ... Variable Not found` (any LUN0 partition missing) → the **LUN0 GPT is
  broken** → step 1. Symptom: nothing boots ("Failed to load image from partition: Load Error"),
  RAM boots start with a black screen, `super`/`userdata` flashes fail with "Partition not found".
- Slots look wrong (current slot says one thing, nothing loads, even `fastboot boot` fails) →
  step 2 (LUN4 GPT).
- If a flash or boot fails with "Load Error" right after other failed attempts, restart the
  bootloader first: `fastboot reboot bootloader` (ABL gets into a bad state).

## 1. Repair the LUN0 GPT (keeps esp + win)

```sh
fastboot flash partition:0 /mnt/mui/ios-topaz/gpt_both0_20261001.bin
fastboot reboot bootloader
fastboot getvar partition-size:win      # must print 0x247F2FB000 again
```

Do **not** use the ROM's `images/gpt_both0.bin`: it is the stock layout without `esp`/`win`.
If the layout ever changes, rebuild the file from fresh dumps of the first and last 1 MiB of
LUN0 (`/dev/block/sda`, e.g. from TWRP) with `mkgptboth.py HEAD TAIL OUT`.

## 2. Repair the LUN4 GPT (A/B slot GUIDs vs attributes)

```sh
fastboot flash partition:4 ~/work/topaz/gpt_both4_20260930.bin   # slot a active, consistent
fastboot reboot bootloader
```

Switch slots **only** with `fastboot set_active a|b` (never edit GPT attributes by hand).

## 3. Flash the stock ROM (no GPT, no esp/win)

```sh
cd /mnt/mui/ios-topaz/rom && tar xzf tapas_global_images_*.tgz
R=$PWD/tapas_global_images_OS2.0.208.0.VMTMIXM_15.0
# the stock script: (a) checks product == tapas but the phone says topaz,
# (b) uses `dirname $0` paths -> run a fixed copy:
sed '1,2d' $R/flash_all_except_data_storage.sh | sed "s|\`dirname \$0\`|$R|g; s|\$(dirname \$0)|$R|g" > /tmp/flash_stock.sh
grep -nE "gpt|partition|esp|win" /tmp/flash_stock.sh    # must print nothing dangerous
bash /tmp/flash_stock.sh
```

The script flashes all firmware (xbl/abl/tz/...) and Android partitions on both slots, then
`set_active a`, `oem erase-vb-index`, reboot. Expected harmless failures: `erase misc/opcust/opconfig`
("Check device console"). If `super`/`userdata`/`vbmeta_system_*` fail with "Partition not found",
LUN0's GPT is broken → step 1, then flash just those:

```sh
I=$R/images
for x in "vbmeta_system_a vbmeta_system.img" "vbmeta_system_b vbmeta_system.img" "rescue rescue.img" \
         "super super.img" "cust cust.img" "userdata userdata.img"; do set -- $x; fastboot flash $1 $I/$2; done
fastboot set_active a && fastboot oem erase-vb-index && fastboot reboot
```

Note: the ROM also overwrites `boot_b` (the Mu UEFI) and `recovery_a/b` (TWRP).

## 4. First boot: "Couldn't initialize user 0"

The phone lands in HyperOS recovery because `userdata` is not an Android filesystem (it held the
iOS HFS+ root, or the ROM's small userdata.img). In recovery: **Wipe Data → Wipe All Data**, then
Reboot → System. Only `userdata` is wiped; `esp`/`win` are separate partitions.

## 5. TWRP (for adb root / backups)

```sh
fastboot flash recovery_a ~/twrp.img
fastboot reboot recovery          # adb shows "recovery", product twrp_tapas
```

## 6. Windows: back up / restore

Back up (TWRP, adb root):

```sh
cd /mnt/mui/winbackup
adb exec-out "dd if=/dev/block/by-name/esp bs=4M" | zstd -T0 -3 -o esp.img.zst
adb exec-out "dd if=/dev/block/by-name/win bs=4M" | zstd -T0 -3 -o win.img.zst
```

Restore (TWRP):

```sh
zstd -dc esp.img.zst | adb shell "dd of=/dev/block/by-name/esp bs=4M"
zstd -dc win.img.zst | adb shell "dd of=/dev/block/by-name/win bs=4M"
```

Boot Windows again: put Mu back into `boot_b` and make slot b active (the menu then offers
Windows / Android):

```sh
fastboot flash boot_b ~/work/win/uefi/Mu-topaz-v10-RELEASE.img
fastboot set_active b && fastboot reboot
# or one-off without flashing: fastboot boot ~/work/win/uefi/Mu-topaz-v10-RELEASE.img
```

## Never

- `fastboot flash partition ...` with the ROM's `gpt_*.bin` (wipes the esp/win layout).
- `fastboot erase`/`format` of `esp` or `win`.
- Hand-editing GPT A/B attributes.

## What broke the phone on 2026-10-18/19 (so it does not happen again)

The iOS port's UFS driver wrote to LUN0 and destroyed its GPT (all LUN0 partitions vanished: super,
userdata, persist, esp, win). After that ABL could not boot anything. Until that driver is fixed,
the iOS kernel must not write to the disk (boot it without `ufsrw=1 rootrw=1`), and a fresh GPT
dump of LUN0 should be taken before every iOS test.
