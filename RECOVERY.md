# sapphire: getting back to recovery

Redmi Note 13 4G (`sapphire` / `sapphiren`), SM6225, A/B slots, unlocked bootloader. All steps
run in **fastboot** (Power + Volume down). Nothing here touches `userdata`, so the iOS root survives.

## What was observed (2026-10-20)

- In recovery mode ABL boots the **kernel in `boot`** of the active slot:
  - with an iOS shim image flashed into `boot`, `fastboot reboot recovery` started iOS;
  - with a kernel from a different ROM build (Xiaomi.eu `boot.img` while another ROM was
    installed), recovery showed the logo for an instant and fell back to fastboot;
  - flashing the **`boot.img` of the installed ROM** (here Lunaris 3.12, extracted from its zip's
    `payload.bin`) into `boot_a` brought OrangeFox back.
- Every failed boot marks the slot unbootable; with both slots unbootable ABL goes straight to
  fastboot (`slot-unbootable:a/b: yes`).
- `recovery_a` is `0x6400000` bytes. Recovery images built for another phone (for example the
  upstream topaz OrangeFox) did not boot with `fastboot boot`; use a sapphire build.

## Diagnose

```sh
fastboot getvar current-slot
fastboot getvar slot-unbootable:a
fastboot getvar slot-retry-count:a
```

## Fix

```sh
fastboot set_active a                      # the slot the ROM is installed in
fastboot flash boot_a ROM-boot.img         # boot.img of that ROM build
fastboot reboot recovery
```

If you only have the ROM zip, extract `boot.img` from `payload.bin` (AOSP
`system/update_engine/scripts/update_payload`, or any payload dumper).

Do not wipe or format data in recovery: `userdata` is the iOS root.

## Booting iOS without losing recovery

Always `fastboot boot sapphire-boot.img` (RAM only). To leave iOS, use Settings → General →
Shut Down, then Power + Volume down for fastboot.
