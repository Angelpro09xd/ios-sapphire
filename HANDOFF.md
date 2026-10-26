# Where this port stands, and how to pick it up again

Written 2026-10-21, when the serverhive machine was about to be migrated. Everything in this
file is about **sapphire / sapphiren** (Redmi Note 13 4G, SM6225 "khaje"), the phone this fork
targets. `README.md` explains the port itself; `NOTES.md` is the dated engineering log, newest
entry first; this file is the short version plus what lives outside git.

## What works on the phone today

Updated 2026-10-26 (kernel 0234; details in `NOTES.md`, entry "SAPPHIRE 2026-10-25/26").

- Boots to the iOS home screen on 8 cores (4K-page kernel, Codemagic workflow `xnu-4k`),
  through our UEFI (`uefi/`), with touch, GPU (Adreno 610, Mesa/turnip + EAGL bridge), sound,
  battery, Wi-Fi firmware, USB networking (`192.168.7.2`).
- Telephony: SIM, signal bars, operator name (ccbridge inside CommCenter).
- **Mobile data (LTE) for every app**, through the IPA driver (`ipa=1`) and `pdp_ip0`; the data
  call starts by itself after the PIN; the Control Center data switch works.
- The panel turns off/on with the power button and auto-lock (no `sweetfb_stayon` any more).
- Open: the panel goes black after the Apple logo until the boot ends; USB link stalls.

## The pieces outside this repository

| What | Where it was | How to get it again |
| --- | --- | --- |
| The iOS root image (16 GB) and its sparse copy | the user's WSL, `~/ios-build/` | rebuild with `tools/sapphire-root.sh` from the iPhone X 16.7.16 IPSW |
| `research/ipsw-x/d22ap.adt` | in this backup | `ipsw img4 im4p extract` on the IPSW's DeviceTree.d22ap.im4p |
| Built kernels (`xnu/out-cm*/`) | in this backup, the newest one | a Codemagic build of this branch, about 4 minutes |
| `a630_sqe.fw`, `a610_zap.*` | in this backup under `gpu-fw/vendor/` | the phone's own `/vendor/firmware`, see below (linux-firmware's `qcom/a630_sqe.fw` also works, probably) |
| The phone's device trees (`ios-sapphire-research/`) | in this backup | dump them from the phone again |
| topaz-woa (reference for the hardware) | cloned under `ios-sapphire-research/` | `git clone https://github.com/MakrSas/topaz-woa` |
| The signed `a610_zap` shader | **found**, in this backup under `gpu-fw/vendor/` | see "Getting the GPU firmware off the phone" |

## How the machines fit together

- The **server** holds this repository, the research dumps and the built kernels, and reaches the
  phone and the user's WSL through two reverse SSH tunnels the user opens from their PC:
  `.adb.sock` (adb) and `.wsl.sock` (their WSL). `tools/bin/adbt` and `tools/bin/wsl` use them.
- **Windows adb** is at `/mnt/c/adb/adb.exe` inside WSL. Do not pipe binary data into
  `adb.exe shell`: it corrupts it. Use `adb push` and `dd` on the phone, and add `< /dev/null`
  to any `adb.exe` call inside a `while read` loop, or it eats the loop's input.
- **USB to WSL** needs usbipd-win on the PC: `usbipd bind --force --busid <id>` once, then
  `usbipd attach --wsl --busid <id>` after every boot. In WSL the interface shows up as `enx...`
  and needs `ip addr add 192.168.7.1/24` each time.
- **Kernels** build on Codemagic (`codemagic.yaml`, free macOS minutes); GitHub Actions is
  locked on this account for billing. The user starts a build and pastes its SSH-access line;
  the kernel is at `~/clone/dxb/fakeroot/System/Library/Kernels/` and can be fetched over that
  SSH before the build's long cleanup, which the user then cancels to save minutes.

## Working on the phone without copying 16 GB

The iOS root is a journaled HFS+ volume in `userdata`. To change a file in it:

1. Shut the phone down from **Settings → General → Shut Down** so the journal is clean
   (`tools/hfs-journal-replay.py` replays it offline if a crash left it dirty), then boot recovery.
2. Copy the phone's HFS+ metadata ranges into a copy of the image on the PC, change files there,
   and write back only the newly allocated blocks plus the metadata
   (`tools/hfs-push-ranges.py`, `tools/sapphire-ssh-install.sh` does the whole thing for sshd).
3. `tools/mbhash.c` hashes every MiB of a partition in one pass (16 GB in under 30 s on the
   phone), which is how the root was once reset to its original image by writing only 1.9 GB.

## Getting the GPU firmware off the phone

Qualcomm's GPU firmware is not in this repository and must not be put in it. It lives in the
phone's own `/vendor/firmware`, which recovery cannot mount: `vendor` is a logical partition
inside `super`, EROFS, and **the live one is in slot `_b`** (recovery only maps the `_a` set, and
those extents hold stale bytes that look like noise, which is misleading). From recovery:

```sh
adb shell lpdump -s 1 /dev/block/sda8     # slot 1: vendor_b, "linear super <first sector>", 1100984 sectors
adb exec-out "dd if=/dev/block/sda8 bs=512 skip=5760040 count=1100984 2>/dev/null" > vendor_b.img
fsck.erofs --extract=vendor-extract --no-preserve --overwrite vendor_b.img   # erofs-utils, no --path in 1.7.1
```

`vendor-extract/firmware/` then has `a610_zap.mdt`, `.b00`, `.b01`, `.b02`, `.elf` and the
phone's own `a630_sqe.fw` (32504 bytes; linux-firmware's is 34188, and topaz-woa preferred the
vendor's). Check the sector offset with `lpdump` instead of trusting the number above. The mdt
has three program headers: a 148-byte PT_NULL, the hash segment (`.b01`, segment type 2 in the
flags) and one PT_LOAD at `0x5000`, 2096 bytes, which is `.b02` — so `zap_load()` loading only
`.b02` at `p_paddr` and asking TrustZone for `max_addr - min_addr` = 0x1000 is right, and matches
Linux `qcom_mdt_load`.

## Booting

Always `fastboot boot <image>`; never flash iOS into `boot`. On this phone the recovery runs with
the kernel from `boot`, so an iOS image there takes the recovery away (`RECOVERY.md` has the way
back, and the ROM's own `boot.img` is what `boot_a` needs).

```sh
tools/sapphire-boot.sh --gpu-fw ../gpu-fw xnu/out-cm6/kernel.development.sweet out.img \
    "sweetusb=2 gpu=2 gpu_delay=60"
```

## The next step

1. The black panel after the Apple logo (0233 removed the minutes-long contiguous allocation
   stall, the symptom stayed): read a full boot log (`dmesg`, UDP 5140) for what the
   framebuffer does between the logo and SpringBoard.
2. SMS and calls in the apps, the USB stall, then CommCenter's own data contexts (4G icon,
   per-app cellular policy).
