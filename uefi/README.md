# UEFI for sapphire

A port of [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to the Redmi Note 13 4G
(sapphire/sapphiren, SM6225), made from `tapasPkg` (Redmi Note 12 4G, the same SoC package `DivarPkg`),
after HackiOS's way of starting XNU from Silicium (`hackios-xnu-kernel/uefi`, BSD-2-Clause-Patent).

Chain: ABL `fastboot boot` -> BootShim -> UEFI -> `SapphireXnuBoot.c` -> our XNU shim (`shim/`, with
the kernel payload behind it) -> XNU. Nothing is flashed.

What is here:

- `BASE` -- the Mu-Silicium commit (the one HackiOS uses);
- `mu-silicium.patch` -- BootShim copies an "XNUP" container that follows the FD to `PAYLOAD_BASE`
  (0xA0000000; 0x70000000 hung the BootShim), `build_uefi.py` passes `payload_base` from the device
  config, BootManagerLib calls `SapphireXnuBoot()` from the BDS entry (no boot menu, no "no OS" screen);
- `files/` -- `Platforms/Xiaomi/sapphirePkg` (memory map from sapphire's own reserved-memory: tapas put
  its DXE heap inside sapphire's `removed_region` 0x60000000-0x65B00000 and hung after the banner),
  `Resources/Configs/sapphire.toml`, `SapphireXnuBoot.c` (leaves the boot services, cleans the caches
  by set/way, MMU off, enters the shim at EL1 with x0 = the device tree, as ABL does);
- `mkuefi-v4.py` -- Silicium's v1 image -> a v4 boot image this ABL accepts (v1/v2 are refused), with
  the BootShim's ARM64 header as our shim's (text_offset 0, flags 0xa: with Silicium's text_offset
  0x5FC00000 and flags 0 the ABL said "Booting OKAY" and stayed in fastboot with the screen off),
  plus the XNUP container (shim Image, device tree).

Not in git: the UEFI driver binaries are tapas's (`Binaries/tapas`, Silicium's submodule); the device
tree is the phone's own (`/sys/firmware/fdt` from Android) as `Resources/DTBs/sapphire.dtb`; ACPI
tables are tapas's, copied to `Silicium-ACPI/Platforms/Xiaomi/sapphire`.

## Build

```sh
git clone https://github.com/Project-Silicium/Mu-Silicium && cd Mu-Silicium
git checkout $(cat ../uefi/BASE) && git submodule update --init --recursive
git apply ../uefi/mu-silicium.patch && cp -r ../uefi/files/* .
cp -r Silicium-ACPI/Platforms/Xiaomi/tapas Silicium-ACPI/Platforms/Xiaomi/sapphire
cp /path/to/sapphire-live.dtb Resources/DTBs/sapphire.dtb
python3 -m venv ../venv && ../venv/bin/pip install -r pip-requirements.txt
# nasm without NuGet/Mono: see hackios-xnu-kernel/uefi/README.md
python3 build_uefi.py -d sapphire -r RELEASE          # -r DEBUG writes the UEFI log on the screen
tools/sapphire-boot.sh ... KERNEL out.img "ARGS"         # builds shim/out/Image (shim + payload)
python3 ../uefi/mkuefi-v4.py Mu-sapphire.img sapphire-uefi.img ../shim/out/Image Resources/DTBs/sapphire.dtb
fastboot boot sapphire-uefi.img
```

If a test leaves the phone in fastboot with a black screen, `fastboot reboot bootloader` (or flashing the
Android boot back) brings the bootloader's screen back.
