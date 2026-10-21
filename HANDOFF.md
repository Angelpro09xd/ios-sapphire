# Where this port stands, and how to pick it up again

Written 2026-10-21, when the serverhive machine was about to be migrated. Everything in this
file is about **sapphire / sapphiren** (Redmi Note 13 4G, SM6225 "khaje"), the phone this fork
targets. `README.md` explains the port itself; `NOTES.md` is the dated engineering log, newest
entry first; this file is the short version plus what lives outside git.

## What works on the phone today

- Boots to the iOS home screen; touch calibrated for this phone (patch 0140); Settings opens.
- USB networking (patch 0142, `sweetusb=2`): the phone is `192.168.7.2`, the PC `192.168.7.1`,
  and the kernel console streams to UDP 5140. This is the fastest way to watch a boot.
- Modem and Wi-Fi firmware boot from the stock `modem_a` (patch 0143), and the phone **joins a
  WPA2 network** with upstream's station code plus `wifi_ssid=` / `wifi_pass=` boot args (0144).
  iOS itself has no network interface on it yet.
- GPU (Adreno 610, patch 0145): the clock dump of stage 1 has run on the phone and matches
  topaz-woa's measurements exactly. Powering it up (`gpu=2`) is the next thing to try.

## The pieces outside this repository

| What | Where it was | How to get it again |
| --- | --- | --- |
| The iOS root image (16 GB) and its sparse copy | the user's WSL, `~/ios-build/` | rebuild with `tools/sapphire-root.sh` from the iPhone X 16.7.16 IPSW |
| `research/ipsw-x/d22ap.adt` | in this backup | `ipsw img4 im4p extract` on the IPSW's DeviceTree.d22ap.im4p |
| Built kernels (`xnu/out-cm*/`) | in this backup, the newest one | a Codemagic build of this branch, about 4 minutes |
| `a630_sqe.fw` | in this backup under `gpu-fw/` | `qcom/a630_sqe.fw` of linux-firmware |
| The phone's device trees (`ios-sapphire-research/`) | in this backup | dump them from the phone again |
| topaz-woa (reference for the hardware) | cloned under `ios-sapphire-research/` | `git clone https://github.com/MakrSas/topaz-woa` |
| The signed `a610_zap` shader | never obtained | only exists on the phone, in the vendor filesystem |

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

## Booting

Always `fastboot boot <image>`; never flash iOS into `boot`. On this phone the recovery runs with
the kernel from `boot`, so an iOS image there takes the recovery away (`RECOVERY.md` has the way
back, and the ROM's own `boot.img` is what `boot_a` needs).

```sh
tools/sapphire-boot.sh --gpu-fw ../gpu-fw xnu/out-cm6/kernel.development.sweet out.img \
    "sweetusb=2 gpu=2 gpu_delay=60"
```

## The next step

Boot `sapphire-gpu2d.img` (in this backup), attach the USB to WSL, and watch UDP 5140 for
`SapphireGpu` lines: the power-up votes GPLL0, brings up the CX domain, the GPU CC branches, the
GMU clock and the SMMU vote, then the GX domain and the core clock at 300 MHz, and finally reads
`RBBM_STATUS` and the always-on counter, which should grow by about 192000 in 10 ms. `gpu_delay`
is what puts that log in the live console instead of only in the ring at the end of `userdata`.

If that works, `gpu=4` runs a command on the GPU with only the open microcode. `gpu=3` needs this
phone's signed zap shader, which still has to be found in its vendor filesystem.
